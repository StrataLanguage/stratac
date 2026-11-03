/* Builder: Assignment. */
#include "Codegen/Builder/BuilderInternal.h"

Value EmitAssign(Builder* b, AssignExpr* n)
{
    /* Impl property write (`cam.FOV = v`): lower to the setter extern call
       (compound assigns are rejected in sema). */
    if (n->target->kind == NodeMember && n->op == AssignSet)
    {
        bool isProp = false;
        Value prop = EmitImplProperty(b, (MemberExpr*)n->target, n->value, &isProp);

        if (isProp)
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
            /* SIMD lane write: `v.x = f` (or `+=` etc.) — load the vector,
               insert the lane, store it back through the same slot/cell. */
            if (lvalue.vectorLane)
            {
                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, lvalue.vectorType, lvalue.ptr, "lanev");
                Value result = Coerce(b, rhs, lvalue.typeDesc);

                if (n->op != AssignSet)
                {
                    LLVMValueRef laneVal
                        = LLVMBuildExtractElement(b->m_builder, cur, LLVMConstInt(I32Ty(b), lvalue.lane, 0), "lanecur");

                    switch (n->op)
                    {
                    case AssignAdd:
                        result = ValueMake(LLVMBuildFAdd(b->m_builder, laneVal, result.value, "add"), lvalue.typeDesc);
                        break;
                    case AssignSub:
                        result = ValueMake(LLVMBuildFSub(b->m_builder, laneVal, result.value, "sub"), lvalue.typeDesc);
                        break;
                    case AssignMul:
                        result = ValueMake(LLVMBuildFMul(b->m_builder, laneVal, result.value, "mul"), lvalue.typeDesc);
                        break;
                    case AssignDiv:
                        result = ValueMake(LLVMBuildFDiv(b->m_builder, laneVal, result.value, "div"), lvalue.typeDesc);
                        break;
                    case AssignMod:
                        result = ValueMake(LLVMBuildFRem(b->m_builder, laneVal, result.value, "mod"), lvalue.typeDesc);
                        break;
                    default:
                        break;
                    }
                }

                LLVMValueRef newVec = LLVMBuildInsertElement(b->m_builder, cur, result.value,
                                                             LLVMConstInt(I32Ty(b), lvalue.lane, 0), "laneins");
                LLVMBuildStore(b->m_builder, newVec, lvalue.ptr);

                return result;
            }

            /* Whole-array / string rebind: free the old buffer, take the new
               {ptr,len} struct, and null a moved source. A string literal RHS
               is a borrowed constant, so it is heap-copied first; a `cstring`
               RHS likewise (borrowed .rodata bytes — the slot owns a copy). */
            if (TypeDescHasArrayShape(&lvalue.typeDesc) && n->op == AssignSet)
            {
                if (rhs.typeDesc.isCString)
                {
                    rhs = ValueMake(BuildOwnedStringFromCStr(b, rhs.value), lvalue.typeDesc);
                }
                else if (lvalue.typeDesc.isString && n->value->kind == NodeStrLiteral)
                {
                    LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, rhs.value, 0, "aslit.p");
                    LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, rhs.value, 1, "aslit.l");
                    rhs = ValueMake(BuildOwnedStringFat(b, sp, sl), rhs.typeDesc);
                }

                LLVMValueRef oldArr = SaveOldValue(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                Node* movedNode = MovedSource(n->value);

                if (movedNode)
                {
                    LValue src = EmitLValueForNullStore(b, movedNode);

                    if (src.valid && TypeDescHasArrayShape(&src.typeDesc))
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
                    }
                }

                EmitDropOne(b, oldArr, lvalue.typeDesc);

                return ValueMake(rhs.value, lvalue.typeDesc);
            }

            /* `=` rebinds the box only when RHS is a box of the SAME kind
               (string=string, ^T=^T); any other assign into a ^T - a plain
               T value (`x = 5;`) or a string into ^string - mutates its
               contents instead. */
            bool rhsIsSameBoxKind
                = TypeDescIsBox(&rhs.typeDesc)
                  && ((lvalue.typeDesc.boxInner == NULL && rhs.typeDesc.boxInner == NULL)
                      || (lvalue.typeDesc.boxInner && rhs.typeDesc.boxInner
                          && strcmp(lvalue.typeDesc.boxInner->name, rhs.typeDesc.boxInner->name) == 0));
            bool boxMove = TypeDescIsBox(&lvalue.typeDesc) && n->op == AssignSet && rhsIsSameBoxKind;
            bool boxMoveFromLiteral = boxMove && !lvalue.typeDesc.boxInner && n->value->kind == NodeStrLiteral;

            if (boxMove && !boxMoveFromLiteral)
            {
                /* Box move: take the pointer, null the source, drop the old
                   value, rebind. The SOURCE is detached BEFORE the drop so the
                   drop glue (and the store) never touch freed memory - for
                   `cur = cur.next` the source slot lives inside the freed object. */
                Node* movedSourceNodePre = MovedSource(n->value);
                LValue srcPre = movedSourceNodePre ? EmitLValueForNullStore(b, movedSourceNodePre) : (LValue){0};

                if (srcPre.valid)
                {
                    LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), srcPre.ptr);
                }

                LLVMValueRef oldBox = SaveOldValue(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);
                EmitDropOne(b, oldBox, lvalue.typeDesc);

                return rhs;
            }

            if (boxMoveFromLiteral)
            {
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                StrLiteral* lit = AsNode(StrLiteral, n->value);
                size_t copyLen = lit->length + 1;
                LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                LLVMValueRef args2[1] = {size};
                StrataAllocFn(b);
                LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args2, 1, "stra");
                LLVMBuildStore(b->m_builder, heap, lvalue.ptr);
                LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
                LLVMValueRef srcGep = rhs.value;
                LLVMValueRef dstGep = heap;
                for (size_t ci = 0; ci < copyLen; ci++)
                {
                    LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                    LLVMValueRef srcIdx[1] = {ciVal};
                    LLVMValueRef dstIdx[1] = {ciVal};
                    LLVMValueRef byteVal = LLVMBuildLoad2(
                        b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "srcas"), "bas");
                    LLVMBuildStore(b->m_builder, byteVal,
                                   LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "dstas"));
                }

                return rhs;
            }

            /* Optional (`T?`) slot receiving a NON-box value: whole-slot
               rebind. The old value is dropped (EmitDropOne handles empty/
               NULL), then a fresh cell is allocated and the owned inner
               constructed into it. Same-kind box RHS took the boxMove path. */
            bool optRebind = TypeDescIsOptional(&lvalue.typeDesc) && n->op == AssignSet;

            if (optRebind)
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
                    LLVMBuildStore(b->m_builder, inner, cell);
                    LLVMBuildStore(b->m_builder, cell, lvalue.ptr);
                    EmitDropOne(b, oldOpt, lvalue.typeDesc);

                    return ValueMake(cell, lvalue.typeDesc);
                }

                /* String inner: construct the owned string (heap copy), then
                   wrap it in a cell - the boxed representation is slot -> cell
                   -> chars, as drop glue and reads expect. Array inner cannot
                   occur (optionals of dynamic arrays are rejected by sema). */
                LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, innerTn);

                LLVMValueRef cellSz = SizeOfConst(b, b->m_ptrTy);
                LLVMValueRef cellArgs[1] = {cellSz};
                StrataAllocFn(b);
                LLVMValueRef strCell
                    = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, cellArgs, 1, "optstr");
                LLVMBuildStore(b->m_builder, owned, strCell);
                LLVMBuildStore(b->m_builder, strCell, lvalue.ptr);
                EmitDropOne(b, oldOpt, lvalue.typeDesc);

                return ValueMake(strCell, lvalue.typeDesc);
            }

            if (TypeDescIsBox(&lvalue.typeDesc) && !boxMove)
            {
                /* Assigning a plain T (or compound-assign) into a ^T mutates
                   its contents in place, not a move - so `x = 5;` and `val -=
                   amt;` work even through a `ref ^T` param or box global.
                   lvalue.ptr is the box slot; load it, then read/write through. */
                LLVMValueRef boxPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lvalue.ptr, "box");
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
                    LLVMBuildStore(b->m_builder, owned, boxPtr);
                    return ValueMake(owned, innerTd);
                }

                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, innerTd.type, boxPtr, "boxval");
                Value result = Coerce(b, rhs, innerTd);
                bool flt = innerTd.isFloat;

                switch (n->op)
                {
                case AssignAdd:
                    result = ValueMake(flt ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                                           : LLVMBuildAdd(b->m_builder, cur, result.value, "add"),
                                       innerTd);
                    break;
                case AssignSub:
                    result = ValueMake(flt ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                                           : LLVMBuildSub(b->m_builder, cur, result.value, "sub"),
                                       innerTd);
                    break;
                case AssignMul:
                    result = ValueMake(flt ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                                           : LLVMBuildMul(b->m_builder, cur, result.value, "mul"),
                                       innerTd);
                    break;
                case AssignDiv:
                    result
                        = ValueMake(flt ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                                        : (innerTd.isUnsigned ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                                                              : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")),
                                    innerTd);
                    break;
                case AssignMod:
                    result
                        = ValueMake(flt ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                                        : (innerTd.isUnsigned ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                                                              : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")),
                                    innerTd);
                    break;
                default:
                    break;
                }

                LLVMBuildStore(b->m_builder, result.value, boxPtr);

                return result;
            }

            /* Element write into an owning-element array (`Rec[]`, `string[]`):
               drop the old element in place, then construct the owned value
               into the slot (heap-copy a string literal, move a movable
               source, take a temporary bitwise). */
            bool elemOwningSlot
                = n->op == AssignSet
                  && (lvalue.typeDesc.isString
                      || (lvalue.typeDesc.structTypeName
                          && TypeRegistryIsOwningStruct(&b->m_registry, lvalue.typeDesc.structTypeName)));

            if (elemOwningSlot && BoxIntoInlineOwning(b, lvalue.typeDesc, rhs.typeDesc))
            {
                /* `holder.field = box;` with an inline owning field: the value moves out of its box. */
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                Value moved = TakeBoxContents(b, rhs, n->value);
                LLVMBuildStore(b->m_builder, moved.value, lvalue.ptr);

                return ValueMake(moved.value, lvalue.typeDesc);
            }

            if (elemOwningSlot)
            {
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);

                TypeName elemTn = lvalue.typeDesc.isString ? *StringTypeName(b)
                                                           : TypeNameLeaf((char*)lvalue.typeDesc.structTypeName);
                LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, &elemTn);
                LLVMBuildStore(b->m_builder, owned, lvalue.ptr);

                return ValueMake(owned, lvalue.typeDesc);
            }

            Value result = Coerce(b, rhs, lvalue.typeDesc);

            if (n->op != AssignSet)
            {
                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, lvalue.typeDesc.type, lvalue.ptr, "cur");
                bool flt = lvalue.typeDesc.isFloat;

                switch (n->op)
                {
                case AssignAdd:
                    result = ValueMake(flt ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                                           : LLVMBuildAdd(b->m_builder, cur, result.value, "add"),
                                       lvalue.typeDesc);
                    break;
                case AssignSub:
                    result = ValueMake(flt ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                                           : LLVMBuildSub(b->m_builder, cur, result.value, "sub"),
                                       lvalue.typeDesc);
                    break;
                case AssignMul:
                    result = ValueMake(flt ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                                           : LLVMBuildMul(b->m_builder, cur, result.value, "mul"),
                                       lvalue.typeDesc);
                    break;
                case AssignDiv:
                    result = ValueMake(flt ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                                           : (lvalue.typeDesc.isUnsigned
                                                  ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                                                  : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")),
                                       lvalue.typeDesc);
                    break;
                case AssignMod:
                    result = ValueMake(flt ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                                           : (lvalue.typeDesc.isUnsigned
                                                  ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                                                  : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")),
                                       lvalue.typeDesc);
                    break;
                default:
                    break;
                }
            }

            LLVMBuildStore(b->m_builder, result.value, lvalue.ptr);

            return result;
        }
    }

    if (n->target->kind == NodeMember)
    {
        /* A vector member write that made it here is either a multi-component
           swizzle (`v.xy = w`) or an out-of-range single lane (`v.z` on a
           float2) — single lanes in range are handled above. */
        MemberExpr* mt = (MemberExpr*)n->target;
        LValue baseLv = EmitLValue(b, mt->base_node);

        if (baseLv.valid
            && (baseLv.typeDesc.isSimdVector
                || (TypeDescIsBox(&baseLv.typeDesc) && baseLv.typeDesc.boxInner
                    && IsNameSimdVector(baseLv.typeDesc.boxInner->name))))
        {
            if (b->m_diag)
            {
                if (mt->member[0] && mt->member[1] == '\0')
                {
                    DiagErrorFmt(b->m_diag, n->base.range, "lane '%s' is out of range for a vector with %d lane(s)",
                                 mt->member,
                                 baseLv.typeDesc.isSimdVector
                                     ? (int)LLVMGetVectorSize(baseLv.typeDesc.type)
                                     : (int)LLVMGetVectorSize(Resolve(b, baseLv.typeDesc.boxInner).type));
                }
                else
                {
                    DiagError(
                        b->m_diag, n->base.range,
                        "swizzle assignment is not supported; assign lanes individually or rebuild the whole vector");
                }
            }

            return rhs;
        }
    }

    if (b->m_diag)
    {
        DiagError(b->m_diag, n->base.range, "assignment to unsupported lvalue");
    }

    return rhs;
}
