/* Builder: Assignment. */
#include "Codegen/Builder/BuilderInternal.h"

/* Impl property write (`cam.FOV = v`): lower to the setter extern call. Returns
   false when the member is not a property, leaving the normal lvalue path to run. */
static bool TryEmitImplPropertyAssign(Builder* b, AssignExpr* n, Value* out)
{
    bool isProp = false;
    Value prop = EmitImplProperty(b, (MemberExpr*)n->target, n->value, &isProp);

    if (isProp)
    {
        *out = prop;
    }

    return isProp;
}

/* A compound assignment (`+=`, `-=`, ...) combines the current value `cur` with the already-coerced
   right-hand side `rhs` in td's arithmetic. Plain `=` (and any other operator) leaves `rhs` as the
   value to store. */
static Value ApplyCompoundOp(Builder* b, AssignOp op, LLVMValueRef cur, Value rhs, TypeDesc td)
{
    bool flt = TypeDescIsFloat(&td);

    switch (op)
    {
    case AssignAdd:
        return ValueMake(flt ? LLVMBuildFAdd(b->builder, cur, rhs.value, "add")
                             : LLVMBuildAdd(b->builder, cur, rhs.value, "add"),
                         td);

    case AssignSub:
        return ValueMake(flt ? LLVMBuildFSub(b->builder, cur, rhs.value, "sub")
                             : LLVMBuildSub(b->builder, cur, rhs.value, "sub"),
                         td);

    case AssignMul:
        return ValueMake(flt ? LLVMBuildFMul(b->builder, cur, rhs.value, "mul")
                             : LLVMBuildMul(b->builder, cur, rhs.value, "mul"),
                         td);

    case AssignDiv:
        return ValueMake(flt ? LLVMBuildFDiv(b->builder, cur, rhs.value, "div")
                             : (TypeDescIsUnsigned(&td) ? LLVMBuildUDiv(b->builder, cur, rhs.value, "div")
                                                        : LLVMBuildSDiv(b->builder, cur, rhs.value, "div")),
                         td);

    case AssignMod:
        return ValueMake(flt ? LLVMBuildFRem(b->builder, cur, rhs.value, "mod")
                             : (TypeDescIsUnsigned(&td) ? LLVMBuildURem(b->builder, cur, rhs.value, "mod")
                                                        : LLVMBuildSRem(b->builder, cur, rhs.value, "mod")),
                         td);

    default:
        return rhs;
    }
}

/* SIMD lane write: `v.x = f` (or `+=` etc.) — load the vector, insert the lane, store
   it back through the same slot. Lanes are floats, so compound operators are float-only. */
static Value EmitVectorLaneAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    LLVMValueRef cur = LLVMBuildLoad2(b->builder, lvalue.vectorType, lvalue.ptr, "lanev");
    Value result = Coerce(b, rhs, lvalue.typeDesc);

    if (n->op != AssignSet)
    {
        LLVMValueRef laneVal
            = LLVMBuildExtractElement(b->builder, cur, LLVMConstInt(I32Ty(b), lvalue.lane, 0), "lanecur");

        switch (n->op)
        {
        case AssignAdd:
            result = ValueMake(LLVMBuildFAdd(b->builder, laneVal, result.value, "add"), lvalue.typeDesc);
            break;
        case AssignSub:
            result = ValueMake(LLVMBuildFSub(b->builder, laneVal, result.value, "sub"), lvalue.typeDesc);
            break;
        case AssignMul:
            result = ValueMake(LLVMBuildFMul(b->builder, laneVal, result.value, "mul"), lvalue.typeDesc);
            break;
        case AssignDiv:
            result = ValueMake(LLVMBuildFDiv(b->builder, laneVal, result.value, "div"), lvalue.typeDesc);
            break;
        case AssignMod:
            result = ValueMake(LLVMBuildFRem(b->builder, laneVal, result.value, "mod"), lvalue.typeDesc);
            break;
        default:
            break;
        }
    }

    LLVMValueRef newVec
        = LLVMBuildInsertElement(b->builder, cur, result.value, LLVMConstInt(I32Ty(b), lvalue.lane, 0), "laneins");
    LLVMBuildStore(b->builder, newVec, lvalue.ptr);

    return result;
}

/* Whole-array / string rebind: free the old buffer, take the new {ptr,len} struct, and null a moved
   source. A string literal RHS is a borrowed constant, so it is heap-copied first; a `cstring` RHS
   likewise (borrowed .rodata bytes — the slot owns a copy). */
static Value EmitArrayRebindAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    if (TypeDescIsCString(&rhs.typeDesc))
    {
        rhs = ValueMake(BuildOwnedStringFromCStr(b, rhs.value), lvalue.typeDesc);
    }
    else if (TypeDescIsString(&lvalue.typeDesc) && n->value->kind == NodeStrLiteral)
    {
        LLVMValueRef sp = LLVMBuildExtractValue(b->builder, rhs.value, 0, "aslit.p");
        LLVMValueRef sl = LLVMBuildExtractValue(b->builder, rhs.value, 1, "aslit.l");
        rhs = ValueMake(BuildOwnedStringFat(b, sp, sl), rhs.typeDesc);
    }

    LLVMValueRef oldArr = SaveOldValue(b, lvalue.ptr, lvalue.typeDesc);
    LLVMBuildStore(b->builder, rhs.value, lvalue.ptr);

    Node* movedNode = MovedSource(n->value);

    if (movedNode)
    {
        LValue src = EmitLValueForNullStore(b, movedNode);

        if (src.valid && TypeDescHasArrayShape(&src.typeDesc))
        {
            LLVMBuildStore(b->builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
        }
    }

    EmitDropOne(b, oldArr, lvalue.typeDesc);

    return ValueMake(rhs.value, lvalue.typeDesc);
}

/* `=` rebinds the box only when RHS is a box of the SAME kind (string=string, ^T=^T); any other assign
   into a ^T - a plain T value (`x = 5;`) or a string into ^string - mutates its contents instead. */
static bool IsSameKindBoxRebind(AssignExpr* n, Value rhs, LValue lvalue)
{
    bool rhsIsSameBoxKind = TypeDescIsBox(&rhs.typeDesc)
                            && ((lvalue.typeDesc.boxInner == NULL && rhs.typeDesc.boxInner == NULL)
                                || (lvalue.typeDesc.boxInner && rhs.typeDesc.boxInner
                                    && strcmp(lvalue.typeDesc.boxInner->name, rhs.typeDesc.boxInner->name) == 0));

    return TypeDescIsBox(&lvalue.typeDesc) && n->op == AssignSet && rhsIsSameBoxKind;
}

/* Box move: take the pointer, null the source, drop the old value, rebind. The SOURCE is detached BEFORE
   the drop so the drop glue (and the store) never touch freed memory - for `cur = cur.next` the source
   slot lives inside the freed object. */
static Value EmitBoxMoveAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    Node* movedSourceNodePre = MovedSource(n->value);
    LValue srcPre = movedSourceNodePre ? EmitLValueForNullStore(b, movedSourceNodePre) : (LValue){0};

    if (srcPre.valid)
    {
        LLVMBuildStore(b->builder, LLVMConstNull(b->ptrTy), srcPre.ptr);
    }

    LLVMValueRef oldBox = SaveOldValue(b, lvalue.ptr, lvalue.typeDesc);
    LLVMBuildStore(b->builder, rhs.value, lvalue.ptr);
    EmitDropOne(b, oldBox, lvalue.typeDesc);

    return rhs;
}

/* A string literal assigned to a ^string: the old value is dropped, then the box gets a heap copy of the
   literal's bytes, since a literal is a borrowed constant. */
static Value EmitBoxFromLiteralAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
    StrLiteral* lit = AS_NODE(StrLiteral, n->value);
    size_t copyLen = lit->length + 1;
    LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
    LLVMValueRef args2[1] = {size};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, args2, 1, "stra");
    LLVMBuildStore(b->builder, heap, lvalue.ptr);
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef srcGep = rhs.value;
    LLVMValueRef dstGep = heap;
    for (size_t ci = 0; ci < copyLen; ci++)
    {
        LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
        LLVMValueRef srcIdx[1] = {ciVal};
        LLVMValueRef dstIdx[1] = {ciVal};
        LLVMValueRef byteVal
            = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, srcGep, srcIdx, 1, "srcas"), "bas");
        LLVMBuildStore(b->builder, byteVal, LLVMBuildGEP2(b->builder, i8Ty, dstGep, dstIdx, 1, "dstas"));
    }

    return rhs;
}

/* Optional (`T?`) slot receiving a NON-box value: whole-slot rebind. The old value is dropped (EmitDropOne
   handles empty/NULL), then a fresh cell is allocated and the owned inner constructed into it. Same-kind box
   RHS took the box-move path. */
static Value EmitOptionalRebindAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    /* Whatever the slot held is dropped once the new value is in (no-op when empty). */
    LLVMValueRef oldOpt = SaveOldValue(b, lvalue.ptr, lvalue.typeDesc);

    const TypeName* innerTn = lvalue.typeDesc.boxInner ? lvalue.typeDesc.boxInner : StringTypeName(b);
    TypeDesc innerTd = Resolve(b, innerTn);

    if (!TypeDescHasArrayShape(&innerTd) && !TypeDescIsBox(&innerTd))
    {
        /* Scalar / struct / handle inner: allocate a cell and
           construct the owned value into it. */
        LLVMValueRef cell = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "optcell");
        LLVMValueRef inner = Coerce(b, rhs, innerTd).value;
        LLVMBuildStore(b->builder, inner, cell);
        LLVMBuildStore(b->builder, cell, lvalue.ptr);
        EmitDropOne(b, oldOpt, lvalue.typeDesc);

        return ValueMake(cell, lvalue.typeDesc);
    }

    /* String inner: construct the owned string (heap copy), then
       wrap it in a cell - the boxed representation is slot -> cell
       -> chars, as drop glue and reads expect. Array inner cannot
       occur (optionals of dynamic arrays are rejected by sema). */
    LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, innerTn);

    LLVMValueRef cellSz = SizeOfConst(b, b->ptrTy);
    LLVMValueRef cellArgs[1] = {cellSz};
    StrataAllocFn(b);
    LLVMValueRef strCell = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, cellArgs, 1, "optstr");
    LLVMBuildStore(b->builder, owned, strCell);
    LLVMBuildStore(b->builder, strCell, lvalue.ptr);
    EmitDropOne(b, oldOpt, lvalue.typeDesc);

    return ValueMake(strCell, lvalue.typeDesc);
}

/* Assigning a plain T (or compound-assign) into a ^T mutates its contents in place, not a move - so
   `x = 5;` and `val -= amt;` work even through a `ref ^T` param or box global. lvalue.ptr is the box
   slot; load it, then read/write through. */
static Value EmitBoxContentAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    LLVMValueRef boxPtr = LLVMBuildLoad2(b->builder, b->ptrTy, lvalue.ptr, "box");
    const TypeName* innerTn = lvalue.typeDesc.boxInner;

    if (!innerTn)
    {
        innerTn = StringTypeName(b);
    }

    TypeDesc innerTd = Resolve(b, innerTn);

    if (BuilderIsOwningValue(b, innerTn))
    {
        /* Content-assigning an OWNING inner (^string = "x",
           ^Rec = arr[i]): drop only the old inner value (free it
            in place, NOT the box allocation), then construct a
            fresh owned inner into the existing box: heap-copy a
            literal, move a movable source. */
        EmitDropOne(b, boxPtr, innerTd);
        LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, innerTn);
        LLVMBuildStore(b->builder, owned, boxPtr);
        return ValueMake(owned, innerTd);
    }

    LLVMValueRef cur = LLVMBuildLoad2(b->builder, innerTd.type, boxPtr, "boxval");
    Value result = Coerce(b, rhs, innerTd);
    result = ApplyCompoundOp(b, n->op, cur, result, innerTd);

    LLVMBuildStore(b->builder, result.value, boxPtr);

    return result;
}

/* Element write into an owning-element array (`Rec[]`, `string[]`) or an inline owning field: drop the old
   element in place, then construct the owned value into the slot (heap-copy a string literal, move a
   movable source, take a temporary bitwise). */
static Value EmitOwningElementAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    if (BoxIntoInlineOwning(b, lvalue.typeDesc, rhs.typeDesc))
    {
        /* `holder.field = box;` with an inline owning field: the value moves out of its box. */
        EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
        Value moved = TakeBoxContents(b, rhs, n->value);
        LLVMBuildStore(b->builder, moved.value, lvalue.ptr);

        return ValueMake(moved.value, lvalue.typeDesc);
    }

    EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);

    TypeName elemTn
        = TypeDescIsString(&lvalue.typeDesc) ? *StringTypeName(b) : TypeNameLeaf((char*)lvalue.typeDesc.structTypeName);
    LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, &elemTn);
    LLVMBuildStore(b->builder, owned, lvalue.ptr);

    return ValueMake(owned, lvalue.typeDesc);
}

/* A plain store, or a compound assignment read-modify-written through the slot. */
static Value EmitScalarStoreAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    Value result = Coerce(b, rhs, lvalue.typeDesc);

    if (n->op != AssignSet)
    {
        LLVMValueRef cur = LLVMBuildLoad2(b->builder, lvalue.typeDesc.type, lvalue.ptr, "cur");
        result = ApplyCompoundOp(b, n->op, cur, result, lvalue.typeDesc);
    }

    LLVMBuildStore(b->builder, result.value, lvalue.ptr);

    return result;
}

/* Assignment into a resolved lvalue, dispatched by what the target holds. */
static Value EmitLValueAssign(Builder* b, AssignExpr* n, Value rhs, LValue lvalue)
{
    if (lvalue.vectorLane)
    {
        return EmitVectorLaneAssign(b, n, rhs, lvalue);
    }

    if (TypeDescHasArrayShape(&lvalue.typeDesc) && n->op == AssignSet)
    {
        return EmitArrayRebindAssign(b, n, rhs, lvalue);
    }

    bool boxMove = IsSameKindBoxRebind(n, rhs, lvalue);
    bool boxMoveFromLiteral = boxMove && !lvalue.typeDesc.boxInner && n->value->kind == NodeStrLiteral;

    if (boxMove && !boxMoveFromLiteral)
    {
        return EmitBoxMoveAssign(b, n, rhs, lvalue);
    }

    if (boxMoveFromLiteral)
    {
        return EmitBoxFromLiteralAssign(b, n, rhs, lvalue);
    }

    if (TypeDescIsOptional(&lvalue.typeDesc) && n->op == AssignSet)
    {
        return EmitOptionalRebindAssign(b, n, rhs, lvalue);
    }

    if (TypeDescIsBox(&lvalue.typeDesc) && !boxMove)
    {
        return EmitBoxContentAssign(b, n, rhs, lvalue);
    }

    bool elemOwningSlot = n->op == AssignSet
                          && (TypeDescIsString(&lvalue.typeDesc)
                              || (lvalue.typeDesc.structTypeName
                                  && TypeRegistryIsOwningStruct(&b->registry, lvalue.typeDesc.structTypeName)));

    if (elemOwningSlot)
    {
        return EmitOwningElementAssign(b, n, rhs, lvalue);
    }

    return EmitScalarStoreAssign(b, n, rhs, lvalue);
}

/* A vector member write that made it here is either a multi-component swizzle (`v.xy = w`) or an
   out-of-range single lane (`v.z` on a float2) — single lanes in range are handled above. Diagnoses
   it and returns true; returns false when the target is not a vector member. */
static bool DiagnoseVectorMemberAssign(Builder* b, AssignExpr* n)
{
    MemberExpr* mt = (MemberExpr*)n->target;
    LValue baseLv = EmitLValue(b, mt->baseNode);

    if (!(baseLv.valid
          && (TypeDescIsSimdVector(&baseLv.typeDesc)
              || (TypeDescIsBox(&baseLv.typeDesc) && baseLv.typeDesc.boxInner
                  && IsNameSimdVector(baseLv.typeDesc.boxInner->name)))))
    {
        return false;
    }

    if (b->diag)
    {
        if (mt->member[0] && mt->member[1] == '\0')
        {
            DiagErrorFmt(b->diag, n->base.range, "lane '%s' is out of range for a vector with %d lane(s)", mt->member,
                         TypeDescIsSimdVector(&baseLv.typeDesc)
                             ? (int)LLVMGetVectorSize(baseLv.typeDesc.type)
                             : (int)LLVMGetVectorSize(Resolve(b, baseLv.typeDesc.boxInner).type));
        }
        else
        {
            DiagError(b->diag, n->base.range,
                      "swizzle assignment is not supported; assign lanes individually or rebuild the whole vector");
        }
    }

    return true;
}

Value EmitAssign(Builder* b, AssignExpr* n)
{
    /* Impl property write (`cam.FOV = v`): lower to the setter extern call
       (compound assigns are rejected in sema). */
    if (n->target->kind == NodeMember && n->op == AssignSet)
    {
        Value prop;

        if (TryEmitImplPropertyAssign(b, n, &prop))
        {
            return prop;
        }
    }

    Value rhs = EmitExpr(b, n->value);

    if (n->target->kind == NodeIdent || n->target->kind == NodeMember || n->target->kind == NodeIndex)
    {
        LValue lvalue = EmitLValue(b, n->target);

        if (lvalue.valid)
        {
            return EmitLValueAssign(b, n, rhs, lvalue);
        }
    }

    if (n->target->kind == NodeMember && DiagnoseVectorMemberAssign(b, n))
    {
        return rhs;
    }

    if (b->diag)
    {
        DiagError(b->diag, n->base.range, "assignment to unsupported lvalue");
    }

    return rhs;
}
