// Builder: Owning boxes (^T and T?): deref, unbox, cells, moves, null stores.
#include "Codegen/Builder/BuilderInternal.h"

// Read through a box pointer without moving or freeing it.
Value DerefBoxValue(Builder* b, Value value)
{
    if (!TypeDescIsBox(&value.typeDesc))
    {
        return value;
    }

    if (!value.typeDesc.boxInner)
    {
        return value;
    }

    TypeDesc innerTd = Resolve(b, value.typeDesc.boxInner);

    // ^T where T is an INCOMPLETE struct (a forward declaration): the box
    // cell holds the T* ITSELF, so unwrapping to T is identity -
    // dereferencing again would read whatever the pointer points at as a
    // pointer. Handles are NOT identity: the cell holds the handle VALUE
    // (a box of a handle allocates like any other), so unwrap loads.
    if (innerTd.structTypeName && TypeRegistryIsOpaque(&b->registry, innerTd.structTypeName)
        && !IsHandleType(&b->registry, innerTd.structTypeName))
    {
        return ValueMake(value.value, innerTd);
    }

    LLVMValueRef loaded = LLVMBuildLoad2(b->builder, innerTd.type, value.value, "boxval");

    // `int v = mkb();`: the box itself is a temporary once its value is read.
    BorrowTemp(b, value, innerTd);

    return ValueMake(loaded, innerTd);
}

// If value is ^T but the target context expects T, load the pointee.
Value UnboxIfBox(Builder* b, Value value, TypeDesc target)
{
    if (TypeDescIsBox(&value.typeDesc) && !TypeDescIsBox(&target) && value.typeDesc.boxInner)
    {
        return DerefBoxValue(b, value);
    }

    return value;
}

// Re-resolves the lvalue of a moved-from node purely to store NULL into it.
// Suppresses the OOB dummy re-construction for array-index sources: the
// scratch's drop+re-init would free a dummy that was legitimately moved out
// (the move loads the value but does not null the scratch itself).
LValue EmitLValueForNullStore(Builder* b, Node* n)
{
    // A piece of a temporary (`mkArr()[0]`, `mkS().name`) has no place to
    // re-resolve (its root is a call): it lives where BorrowTempPart recorded
    // it, inside the temporary. Nulling it there makes the temporary's
    // end-of-statement drop skip the moved piece.
    const TempPart* tp = FindTempPart(b, n);

    if (tp)
    {
        LValue part = {0};
        part.valid = true;
        part.ptr = tp->addr;
        part.typeDesc = tp->td;
        return part;
    }

    b->nullStoreLValue = true;
    LValue src = EmitLValue(b, n);
    b->nullStoreLValue = false;
    return src;
}

// Nulls a moved-from owning slot after its value was transferred: array fats
// zero wholesale, owning-struct slots zero wholesale (canonical empty
// strings/fats/null boxes in every owning field), box/string slots zero the
// pointer.
void NullOwningSlot(Builder* b, LValue src)
{
    if (!src.valid || src.vectorLane)
    {
        return;
    }

    if (TypeDescHasArrayShape(&src.typeDesc))
    {
        LLVMBuildStore(b->builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
    }
    else if (src.typeDesc.structTypeName && TypeRegistryIsOwningStruct(&b->registry, src.typeDesc.structTypeName))
    {
        LLVMBuildStore(b->builder, LLVMConstNull(src.typeDesc.type), src.ptr);
    }
    else
    {
        LLVMBuildStore(b->builder, LLVMConstNull(b->ptrTy), src.ptr);
    }
}

// Allocates a fresh T cell and constructs the owned inner value `src` into it,
// returning the heap pointer. Shared by every ^T / T? box-up site.
LLVMValueRef EmitBoxCell(Builder* b, const TypeName* innerTn, Value src, Node* srcNode, const char* name)
{
    TypeDesc innerTd = Resolve(b, innerTn);
    LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), name);

    LLVMValueRef inner = EmitOwnedValue(b, src, srcNode, innerTn);
    LLVMBuildStore(b->builder, inner, heap);
    return heap;
}

// Deep-copies the CELL behind a non-null box pointer `src` into a fresh
// allocation, returning the new cell pointer. Callers guard optionals.
LLVMValueRef EmitBoxCellCopy(Builder* b, Value src, TypeDesc td)
{
    TypeDesc innerTd = Resolve(b, td.boxInner);
    LLVMValueRef sz = SizeOfConst(b, innerTd.type);
    LLVMValueRef args[1] = {sz};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, args, 1, "cpbox");

    const StructType* st = TypeRegistryFind(&b->registry, td.boxInner->name);
    LLVMTypeRef structTy = st ? (LLVMTypeRef)StrMapGet(&b->structTypes, td.boxInner->name) : NULL;

    if (st && structTy)
    {
        // Field copies go through the cached per-struct copy fn: a REAL
        // call, so a self-referential inner (`Node? next`) recurses at
        // runtime (bounded by the null slots) instead of hanging IR-gen.
        LLVMTypeRef copyParamTys[2] = {b->ptrTy, b->ptrTy};
        LLVMTypeRef copyFnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->ctx), copyParamTys, 2, 0);
        LLVMValueRef copyFn = GetOrCreateStructCopyFn(b, td.boxInner->name);
        LLVMValueRef copyArgs[2] = {heap, src.value};
        LLVMBuildCall2(b->builder, copyFnTy, copyFn, copyArgs, 2, "");
    }
    else
    {
        // Inner is not a registered struct: a scalar, or a bare owning
        // value such as string (^string), another box, or an array.
        // Deep-copy it.
        Value innerVal = ValueMake(LLVMBuildLoad2(b->builder, innerTd.type, src.value, "cv"), innerTd);
        Value copied = EmitCopyValue(b, innerVal, innerTd);
        LLVMBuildStore(b->builder, copied.value, heap);
    }

    return heap;
}

// A `^T` (or a proven `T?`) given where an inline owning `T` is wanted: a struct field, an array
// element.
bool BoxIntoInlineOwning(Builder* b, TypeDesc targetTd, TypeDesc valueTd)
{
    return !TypeDescIsBox(&targetTd) && !TypeDescHasArrayShape(&targetTd) && targetTd.structTypeName
           && TypeRegistryIsOwningStruct(&b->registry, targetTd.structTypeName) && TypeDescIsBox(&valueTd)
           && valueTd.boxInner && strcmp(valueTd.boxInner->name, targetTd.structTypeName) == 0;
}

// Moves the value out of its box cell (see BoxIntoInlineOwning): the cell is freed, and a variable
// it came from is left empty, so its own drop does nothing. Dropping the box instead would drop
// the value that now lives elsewhere.
Value TakeBoxContents(Builder* b, Value box, Node* srcNode)
{
    Value inner = DerefBoxValue(b, box);
    Node* moved = srcNode ? MovedSource(srcNode) : NULL;

    if (moved)
    {
        LValue src = EmitLValueForNullStore(b, moved);

        if (src.valid && !src.vectorLane)
        {
            LLVMBuildStore(b->builder, LLVMConstNull(b->ptrTy), src.ptr);
        }
    }

    LLVMValueRef freeArgs[1] = {box.value};
    StrataFreeFn(b);
    LLVMBuildCall2(b->builder, b->freeFnType, b->freeFn, freeArgs, 1, "");
    return inner;
}

Value EmitNullTest(Builder* b, NullTestExpr* nt)
{
    // `expr?` - test whether the optional holds anything (zero cost
    // beyond the compare; sema guarantees a nullable path). A T?/^T slot
    // tests the pointer; an optional array tests its fat struct: set =
    // non-null data OR non-zero length.
    LValue lv = EmitLValue(b, nt->operand);

    if (lv.valid && TypeDescHasArrayShape(&lv.typeDesc))
    {
        LLVMValueRef data = LLVMBuildLoad2(b->builder, b->ptrTy, lv.ptr, "oad");
        LLVMValueRef len = LLVMBuildLoad2(b->builder, I32Ty(b), ArrayLenPtr(b, lv.ptr), "oal");

        LLVMValueRef dataNonNull = LLVMBuildICmp(b->builder, LLVMIntNE, data, LLVMConstNull(b->ptrTy), "nn");
        LLVMValueRef lenNonZero = LLVMBuildICmp(b->builder, LLVMIntNE, len, LLVMConstInt(I32Ty(b), 0, 0), "nz");

        return ValueMake(LLVMBuildOr(b->builder, dataNonNull, lenNonZero, "opt"), TypeDescMake(I1Ty(b), 0, NULL));
    }

    // Testing a temporary (`Make()?`) only reads it: the end of the statement drops it.
    Value tested = {0};

    if (!lv.valid)
    {
        tested = EmitExpr(b, nt->operand);
        ClaimTemp(b, tested);
    }

    LLVMValueRef slotPtr = lv.valid ? lv.ptr : tested.value;
    LLVMValueRef boxPtr = lv.valid ? LLVMBuildLoad2(b->builder, b->ptrTy, slotPtr, "opt") : slotPtr;

    LLVMValueRef isNonNull = LLVMBuildICmp(b->builder, LLVMIntNE, boxPtr, LLVMConstNull(b->ptrTy), "nn");

    return ValueMake(isNonNull, TypeDescMake(I1Ty(b), 0, NULL));
}

void EmitBoxVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc)
{
    LLVMValueRef slot = EntryAlloca(b, b->ptrTy, "box");

    if (varDecl->init)
    {
        Value value = EmitExpr(b, varDecl->init);
        bool isLiteral = varDecl->init->kind == NodeStrLiteral;

        // Direct move: source and destination are the same owning type
        // (string=string, ^T=^T). Copy the pointer, null the
        // source. No allocation needed.
        bool sameOwningType = TypeDescIsBox(&value.typeDesc) && !isLiteral
                              && ((typeDesc.boxInner == NULL && value.typeDesc.boxInner == NULL)
                                  || (typeDesc.boxInner && value.typeDesc.boxInner
                                      && strcmp(typeDesc.boxInner->name, value.typeDesc.boxInner->name) == 0));

        if (sameOwningType)
        {
            LLVMBuildStore(b->builder, value.value, slot);
            NullMovedSource(b, varDecl->init);
        }
        else if (typeDesc.boxInner)
        {
            // ^T: allocate a T slot, construct the owned inner into it,
            // store. For ^string this is: construct a string (heap-copy
            // literal / move source), then box it up - identical to ^int
            // but the inner happens to be owning.
            LLVMValueRef heap = EmitBoxCell(b, typeDesc.boxInner, value, varDecl->init, "heap");
            LLVMBuildStore(b->builder, heap, slot);
        }
        else
        {
            // string (owning primitive, no box inner): construct the
            // owned value directly into the slot.
            LLVMValueRef owned = EmitOwnedValue(b, value, varDecl->init, StringTypeName(b));
            LLVMBuildStore(b->builder, owned, slot);
        }
    }
    else
    {
        LLVMBuildStore(b->builder, LLVMConstNull(b->ptrTy), slot);
    }

    Value* sym = (Value*)ArenaAlloc(b->arena, sizeof(Value));
    sym->value = slot;
    sym->typeDesc = typeDesc;

    SymDeclare(b, varDecl->name, sym);

    OwnLocal* ol = (OwnLocal*)ArenaAlloc(b->arena, sizeof(OwnLocal));
    ol->slot = slot;
    ol->td = typeDesc;
    VecPush(&b->owningLocals, ol);
}
