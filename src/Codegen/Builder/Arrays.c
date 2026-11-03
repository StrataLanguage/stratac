/* Builder: Dynamic and fixed arrays: layout, element access, copy/zero/drop loops, literals. */
#include "Codegen/Builder/BuilderInternal.h"

/* The shared {ptr, u32 len, u32 cap} fat struct for every T[] and string
   (cached so all arrays share one type). len/cap are u64-truncated views:
   `.length`/`.cap` are `uint`, so 16 bytes covers both fields. */
LLVMTypeRef ArrayStructType(Builder* b)
{
    if (!b->m_arrayType)
    {
        LLVMTypeRef fields[3] = {b->m_ptrTy, I32Ty(b), I32Ty(b)};
        b->m_arrayType = LLVMStructTypeInContext(b->m_ctx, fields, 3, 0);
    }

    return b->m_arrayType;
}

/* GEP helpers for the three array-slot fields: 0 = data ptr, 1 = length, 2 = capacity. */
LLVMValueRef ArrayDataPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, 0)};
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "aptr");
}

LLVMValueRef ArrayLenPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, 1)};
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "alen");
}

LLVMValueRef ArrayCapPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, 2)};
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "acap");
}

/* Widens a u32 length/capacity value to the i64 used by GEP indices, byte
   counts and loop bounds (zero-extend: lengths are never negative). */
LLVMValueRef WidenLen(Builder* b, LLVMValueRef v)
{
    if (LLVMTypeOf(v) == I64Ty(b))
    {
        return v;
    }

    return LLVMBuildZExt(b->m_builder, v, I64Ty(b), "wlen");
}

/* In-bounds element address; aliased rest arrays hold T* slots, so load the slot. */
static LLVMValueRef PlainElemPtr(Builder* b, LLVMValueRef dataPtr, LLVMValueRef idxVal, LLVMTypeRef elemTy,
                                 bool aliased)
{
    LLVMValueRef idxArgs[1] = {idxVal};

    if (aliased)
    {
        LLVMValueRef slotAddr = LLVMBuildGEP2(b->m_builder, b->m_ptrTy, dataPtr, idxArgs, 1, "ais");
        return LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slotAddr, "alias");
    }

    return LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, idxArgs, 1, "ai");
}

/* Resolves arr[idx] under the profile bounds check: boundsCheck off -> raw
   GEP; AOT -> panic branch; JIT -> no abort, report via strata_oob and
   continue with a dummy slot (reads yield a dummy, writes absorbed). Owning
   elements rebuild scratch per event; null-store re-resolution skips that. */
LLVMValueRef EmitCheckedElemPtr(Builder* b, LLVMValueRef idxVal, LLVMValueRef lenVal, LLVMValueRef dataPtr,
                                LLVMTypeRef elemTy, TypeDesc elemTd, bool aliased)
{
    if (!b->m_boundsCheck)
    {
        return PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    }

    if (!b->m_jitMode)
    {
        EmitBoundsCheck(b, idxVal, lenVal);
        return PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, idxVal, lenVal, "oob");

    LLVMValueRef scratch = NULL;

    if (ElemNeedsScratchDummy(b, elemTd))
    {
        scratch = EntryAllocaZeroed(b, elemTd.type ? elemTd.type : b->m_ptrTy, "oob.slot");
    }

    LLVMBasicBlockRef oobBB = NewBb(b, "oob.dummy");
    LLVMBasicBlockRef okBB = NewBb(b, "oob.ok");
    LLVMBasicBlockRef mergeBB = NewBb(b, "oob.elem");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);
    b->m_terminated = true;

    PositionAtEnd(b, oobBB);

    /* A bookkeeping re-resolution of the same expression (null-the-source
       after a move) is not a new access: the access itself already reported. */
    if (!b->m_nullStoreLValue)
    {
        EmitOobReport(b);
    }

    LLVMValueRef dummyAddr;

    if (scratch)
    {
        dummyAddr = scratch;

        if (!b->m_nullStoreLValue)
        {
            EmitDropOne(b, scratch, elemTd);
            EmitDummyInit(b, scratch, elemTd);
        }
    }
    else
    {
        dummyAddr = SharedDummyAddr(b, elemTd);
    }

    Br(b, mergeBB);
    LLVMBasicBlockRef oobEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, okBB);
    LLVMValueRef elemAddr = PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    Br(b, mergeBB);
    LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->m_builder, b->m_ptrTy, "el");
    LLVMValueRef incomingVals[2] = {dummyAddr, elemAddr};
    LLVMBasicBlockRef incomingBlocks[2] = {oobEnd, okEnd};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
    return phi;
}

/* Element address in a fixed inline [N x T]; the leading 0 walks into the array. */
static LLVMValueRef FixedElemPtr(Builder* b, LLVMValueRef arrPtr, LLVMValueRef idxVal, LLVMTypeRef arrTy)
{
    LLVMValueRef idxArgs[2] = {IdxConst(b, 0), idxVal};
    return LLVMBuildGEP2(b->m_builder, arrTy, arrPtr, idxArgs, 2, "ai");
}

/* Bounds-checked variant for fixed arrays. The length is the compile-time
   dimension; elements are never owning (enforced by sema), so the JIT dummy
   is a shared slot and never needs per-site scratch. */
LLVMValueRef EmitCheckedFixedElemPtr(Builder* b, LLVMValueRef idxVal, ArrayLength length, LLVMValueRef arrPtr,
                                     LLVMTypeRef arrTy, TypeDesc elemTd)
{
    LLVMValueRef lenVal = LLVMConstInt(I64Ty(b), (unsigned long long)(length < 0 ? 0 : length), 0);

    if (!b->m_boundsCheck)
    {
        return FixedElemPtr(b, arrPtr, idxVal, arrTy);
    }

    if (!b->m_jitMode)
    {
        EmitBoundsCheck(b, idxVal, lenVal);
        return FixedElemPtr(b, arrPtr, idxVal, arrTy);
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, idxVal, lenVal, "oob");

    LLVMBasicBlockRef oobBB = NewBb(b, "oob.dummy");
    LLVMBasicBlockRef okBB = NewBb(b, "oob.ok");
    LLVMBasicBlockRef mergeBB = NewBb(b, "oob.elem");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);
    b->m_terminated = true;

    PositionAtEnd(b, oobBB);

    if (!b->m_nullStoreLValue)
    {
        EmitOobReport(b);
    }

    LLVMValueRef dummyAddr = SharedDummyAddr(b, elemTd);

    Br(b, mergeBB);
    LLVMBasicBlockRef oobEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, okBB);
    LLVMValueRef elemAddr = FixedElemPtr(b, arrPtr, idxVal, arrTy);
    Br(b, mergeBB);
    LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->m_builder, b->m_ptrTy, "el");
    LLVMValueRef incomingVals[2] = {dummyAddr, elemAddr};
    LLVMBasicBlockRef incomingBlocks[2] = {oobEnd, okEnd};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
    return phi;
}

/* Copies `count` elements from srcData to dstData. */
static void EmitArrayCopyLoop(Builder* b, LLVMValueRef dstData, LLVMValueRef srcData, LLVMValueRef count,
                              LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "acp.cond");
    LLVMBasicBlockRef body = NewBb(b, "acp.body");
    LLVMBasicBlockRef end = NewBb(b, "acp.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "acp.i");
    LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, count, "acplt");
    LLVMBuildCondBr(b->m_builder, cont, body, end);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = {i};
    LLVMValueRef srcEl = LLVMBuildGEP2(b->m_builder, elemTy, srcData, ep, 1, "s");
    LLVMValueRef dstEl = LLVMBuildGEP2(b->m_builder, elemTy, dstData, ep, 1, "d");
    LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, elemTy, srcEl, "e"), dstEl);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, end);
}

/* Zero-initializes elements [start, end) of data (nulls owning slots safely). */
static void EmitArrayZeroLoop(Builder* b, LLVMValueRef data, LLVMValueRef start, LLVMValueRef end, LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "az.cond");
    LLVMBasicBlockRef body = NewBb(b, "az.body");
    LLVMBasicBlockRef endBb = NewBb(b, "az.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "az.i");
    LLVMBuildStore(b->m_builder, start, iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, end, "azlt");
    LLVMBuildCondBr(b->m_builder, cont, body, endBb);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = {i};
    LLVMValueRef el = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "z");
    LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), el);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, endBb);
}

/* Drops owning elements [start, end) of data before the buffer is freed. */
static void EmitArrayDropRange(Builder* b, LLVMValueRef data, LLVMValueRef start, LLVMValueRef end, TypeDesc elemTd,
                               LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "adr.cond");
    LLVMBasicBlockRef body = NewBb(b, "adr.body");
    LLVMBasicBlockRef endBb = NewBb(b, "adr.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "adr.i");
    LLVMBuildStore(b->m_builder, start, iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, end, "adrlt");
    LLVMBuildCondBr(b->m_builder, cont, body, endBb);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = {i};
    LLVMValueRef el = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "drop");
    EmitDropOne(b, el, elemTd);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, endBb);
}

/* Stores `v` (the value of `valNode`) into the array slot `elAddr` of element type `elemTd`, moving it in:
   a movable source is emptied, a literal or borrowed value is copied or boxed as the element needs. */
static void StoreMovedElement(Builder* b, LLVMValueRef elAddr, Value v, Node* valNode, TypeDesc elemTd)
{
    bool sameOwningType = TypeDescIsBox(&v.typeDesc)
                          && ((elemTd.boxInner == NULL && v.typeDesc.boxInner == NULL)
                              || (elemTd.boxInner && v.typeDesc.boxInner
                                  && strcmp(elemTd.boxInner->name, v.typeDesc.boxInner->name) == 0))
                          && valNode->kind != NodeStrLiteral;

    bool stringMove = TypeDescIsString(&elemTd) && TypeDescIsString(&v.typeDesc) && valNode->kind != NodeStrLiteral;
    /* An array pushed into an array of arrays moves: the source is emptied, or both would free it. */
    bool arrayMove = TypeDescHasArrayShape(&elemTd) && !TypeDescIsString(&elemTd) && TypeDescHasArrayShape(&v.typeDesc)
                     && !TypeDescIsString(&v.typeDesc);

    if (sameOwningType || stringMove || arrayMove)
    {
        LLVMBuildStore(b->m_builder, v.value, elAddr);
        NullMovedSource(b, valNode);
    }
    else if (TypeDescIsBox(&elemTd) && elemTd.boxInner)
    {
        /* ^T element from a non-^T value (bare T, string literal
           into ^string): allocate a T slot, construct the owned
           inner value, store it. */
        TypeDesc innerTd = Resolve(b, elemTd.boxInner);
        LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "abox");
        LLVMValueRef inner = EmitOwnedValue(b, v, valNode, elemTd.boxInner);
        LLVMBuildStore(b->m_builder, inner, heap);
        LLVMBuildStore(b->m_builder, heap, elAddr);
    }
    else if (TypeDescIsString(&elemTd))
    {
        /* string element: heap-copy literal, move source. */
        LLVMValueRef owned = EmitOwnedValue(b, v, valNode, StringTypeName(b));
        LLVMBuildStore(b->m_builder, owned, elAddr);
    }
    else if (BoxIntoInlineOwning(b, elemTd, v.typeDesc))
    {
        LLVMBuildStore(b->m_builder, TakeBoxContents(b, v, valNode).value, elAddr);
    }
    else if (elemTd.structTypeName && v.typeDesc.structTypeName
             && strcmp(elemTd.structTypeName, v.typeDesc.structTypeName) == 0
             && TypeRegistryIsOwningStruct(&b->m_registry, elemTd.structTypeName))
    {
        /* Owning-struct element (`Rec[]`): bitwise store = ownership
           transfer; a movable source is zeroed (a struct literal is a
           fresh temporary whose fields the array now owns). */
        LLVMBuildStore(b->m_builder, v.value, elAddr);
        NullMovedSource(b, valNode);
    }
    else
    {
        LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elAddr);
    }
}

/* Inline array helpers: array_push / array_pop / array_resize.
   Each mutates the array through its lvalue and returns push->uint (the
   new length), pop->T, resize->void. Push/resize grow the backing buffer
   geometrically: doubling the capacity when out of room (seed 4). */
Value EmitArrayBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    LValue arr = EmitLValue(b, arg0);

    TypeDesc elemTd = Resolve(b, arr.typeDesc.arrayInner);
    LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

    LLVMValueRef dataPtrPtr = ArrayDataPtr(b, arr.ptr);
    LLVMValueRef lenPtr = ArrayLenPtr(b, arr.ptr);
    LLVMValueRef capPtr = ArrayCapPtr(b, arr.ptr);

    if (strcmp(n->callee, "array_pop") == 0)
    {
        return EmitArrayPop(b, dataPtrPtr, lenPtr, elemTd, elemTy);
    }

    if (strcmp(n->callee, "array_reserve") == 0)
    {
        return EmitArrayReserve(b, n, dataPtrPtr, lenPtr, capPtr, elemTy);
    }

    if (strcmp(n->callee, "array_remove") == 0 || strcmp(n->callee, "array_swap_remove") == 0)
    {
        return EmitArrayRemove(b, n, dataPtrPtr, lenPtr, elemTd, elemTy);
    }

    if (strcmp(n->callee, "array_replace") == 0)
    {
        return EmitArrayReplace(b, n, dataPtrPtr, lenPtr, elemTd, elemTy);
    }

    if (strcmp(n->callee, "array_take") == 0)
    {
        return EmitArrayTake(b, n, dataPtrPtr, lenPtr, elemTd, elemTy);
    }

    if (strcmp(n->callee, "array_push") == 0)
    {
        return EmitArrayPush(b, n, dataPtrPtr, lenPtr, capPtr, elemTd, elemTy);
    }

    return EmitArrayResize(b, n, arr.typeDesc.arrayInner, dataPtrPtr, lenPtr, capPtr, elemTd, elemTy);
}

/* array_pop: the popped element goes to the caller; an empty array stays empty. */
Value EmitArrayPop(Builder* b, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd, LLVMTypeRef elemTy)
{
    LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len");
    LLVMValueRef one = LLVMConstInt(I32Ty(b), 1, 0);
    LLVMValueRef lm1 = LLVMBuildSub(b->m_builder, len, one, "lm1");
    LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");

    /* Popping an empty array is an out-of-bounds read of index len-1
       (wraps to 0xFFFFFFFF): route it through the same bounds check as
       `a[i]` (AOT panics; JIT reports via strata_oob and yields a dummy). */
    LLVMValueRef elAddr = EmitCheckedElemPtr(b, WidenLen(b, lm1), WidenLen(b, len), data, elemTy, elemTd, false);
    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, elemTy, elAddr, "popped");

    /* The popped value now belongs to the caller: clear the vacated slot
       (or the JIT's OOB scratch dummy) so no later drop of it frees the
       value a second time. */
    if (ElemNeedsScratchDummy(b, elemTd))
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), elAddr);
    }

    /* Never underflow: an empty array stays empty. */
    LLVMValueRef isEmpty = LLVMBuildICmp(b->m_builder, LLVMIntEQ, len, LLVMConstInt(I32Ty(b), 0, 0), "pop.empty");
    LLVMBuildStore(b->m_builder, LLVMBuildSelect(b->m_builder, isEmpty, len, lm1, "pop.len"), lenPtr);

    return ValueMake(v, elemTd);
}

/* array_reserve(a, want): grows the capacity to at least `want`; the length and elements are unchanged. */
Value EmitArrayReserve(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, LLVMValueRef capPtr,
                       LLVMTypeRef elemTy)
{
    /* Grows the capacity to at least `want`; the length and elements are unchanged. */
    LLVMValueRef want = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    LLVMValueRef len64 = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len"));
    LLVMValueRef cap64 = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), capPtr, "cap"));
    LLVMValueRef oldData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");

    LLVMBasicBlockRef growBb = NewBb(b, "rsv.grow");
    LLVMBasicBlockRef doneBb = NewBb(b, "rsv.done");
    LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntUGT, want, cap64, "rsv.more"), growBb, doneBb);
    b->m_terminated = true;

    PositionAtEnd(b, growBb);
    LLVMValueRef allocArgs[1] = {LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), want, "rsv.bytes")};
    StrataAllocFn(b);
    LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "rsv.buf");
    EmitArrayCopyLoop(b, newData, oldData, len64, elemTy);
    LLVMValueRef freeArgs[1] = {oldData};
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");
    LLVMBuildStore(b->m_builder, newData, dataPtrPtr);
    LLVMBuildStore(b->m_builder, LLVMBuildTrunc(b->m_builder, want, I32Ty(b), "rsv.cap"), capPtr);
    Br(b, doneBb);

    PositionAtEnd(b, doneBb);
    return ValueMake(NULL, TypeDescMake(NULL, TDF_VOID, NULL));
}

/* array_remove / array_swap_remove(a, i): the element at `i` goes to the caller; swap_remove fills the hole with the
 * last element. */
Value EmitArrayRemove(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                      LLVMTypeRef elemTy)
{
    bool swap = strcmp(n->callee, "array_swap_remove") == 0;

    /* The index first: evaluating it may change the array. */
    LLVMValueRef index = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len");
    LLVMValueRef len64 = WidenLen(b, len);
    LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");

    /* Out of range is the bounds error `a[i]` would be (AOT panics; the JIT reports it), and then
       nothing is removed. */
    EmitCheckedElemPtr(b, index, len64, data, elemTy, elemTd, false);
    LLVMValueRef inRange = LLVMBuildICmp(b->m_builder, LLVMIntULT, index, len64, "rm.in");
    LLVMBasicBlockRef doBb = NewBb(b, "rm.do");
    LLVMBasicBlockRef endBb = NewBb(b, "rm.end");

    /* The removed element goes to the caller, who drops it (or keeps it). Out of range yields a zero. */
    LLVMValueRef removedSlot = EntryAlloca(b, elemTy, "rm.out");
    LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), removedSlot);

    LLVMBuildCondBr(b->m_builder, inRange, doBb, endBb);
    b->m_terminated = true;

    PositionAtEnd(b, doBb);
    LLVMValueRef one64 = LLVMConstInt(I64Ty(b), 1, 0);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, index, one64, "rm.next");
    LLVMValueRef last = LLVMBuildSub(b->m_builder, len64, one64, "rm.last");

    /* The survivors move bitwise: ownership goes with them. */
    LLVMValueRef holeIdx[1] = {index};
    LLVMValueRef hole = LLVMBuildGEP2(b->m_builder, elemTy, data, holeIdx, 1, "rm.hole");
    LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, elemTy, hole, "rm.taken"), removedSlot);

    if (swap)
    {
        LLVMValueRef lastIdx[1] = {last};
        LLVMValueRef lastAddr = LLVMBuildGEP2(b->m_builder, elemTy, data, lastIdx, 1, "rm.lastp");
        LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, elemTy, lastAddr, "rm.moved"), hole);
    }
    else
    {
        LLVMValueRef nextIdx[1] = {next};
        LLVMValueRef after = LLVMBuildGEP2(b->m_builder, elemTy, data, nextIdx, 1, "rm.after");
        EmitArrayCopyLoop(b, hole, after, LLVMBuildSub(b->m_builder, last, index, "rm.count"), elemTy);
    }

    LLVMBuildStore(b->m_builder, LLVMBuildTrunc(b->m_builder, last, I32Ty(b), "rm.len"), lenPtr);
    Br(b, endBb);

    PositionAtEnd(b, endBb);
    return ValueMake(LLVMBuildLoad2(b->m_builder, elemTy, removedSlot, "rm.value"), elemTd);
}

/* array_replace(a, i, v): stores `v` at `i` and returns the old element. */
Value EmitArrayReplace(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                       LLVMTypeRef elemTy)
{
    /* Left to right: the index, then the value (either may change the array). */
    LLVMValueRef index = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    Node* valNode = (Node*)VecGet(&n->args, 2);
    Value v = EmitExpr(b, valNode);

    LLVMValueRef len64 = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len"));
    LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
    LLVMValueRef elAddr = EmitCheckedElemPtr(b, index, len64, data, elemTy, elemTd, false);

    /* The old element goes to the caller; the new one takes its place, ownership and all. */
    LLVMValueRef old = LLVMBuildLoad2(b->m_builder, elemTy, elAddr, "repl.old");
    StoreMovedElement(b, elAddr, v, valNode, elemTd);
    return ValueMake(old, elemTd);
}

/* array_take(a, i): moves the element at `i` out and leaves the empty value in its slot. */
Value EmitArrayTake(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                    LLVMTypeRef elemTy)
{
    LLVMValueRef index = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    LLVMValueRef len64 = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len"));
    LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
    LLVMValueRef elAddr = EmitCheckedElemPtr(b, index, len64, data, elemTy, elemTd, false);

    /* The element goes to the caller; the slot keeps the empty value (all zero for a string, array or optional). */
    LLVMValueRef taken = LLVMBuildLoad2(b->m_builder, elemTy, elAddr, "take.value");
    LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), elAddr);
    return ValueMake(taken, elemTd);
}

/* array_push(a, v): appends `v` and returns the new length. */
Value EmitArrayPush(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, LLVMValueRef capPtr,
                    TypeDesc elemTd, LLVMTypeRef elemTy)
{
    TypeDesc uintTd = TypeDescMake(I32Ty(b), TDF_UNSIGNED, NULL);

    /* Evaluate the pushed value first: it may read the array itself
       (`array_push(a, a[0])`), and the grow path below frees the old
       buffer. */
    Node* valNode = (Node*)VecGet(&n->args, 1);
    Value v = EmitExpr(b, valNode);

    LLVMValueRef one32 = LLVMConstInt(I32Ty(b), 1, 0);
    LLVMValueRef oldLen = LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len");
    LLVMValueRef oldLen64 = WidenLen(b, oldLen);
    LLVMValueRef oldCap = LLVMBuildLoad2(b->m_builder, I32Ty(b), capPtr, "cap");
    LLVMValueRef oldData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");

    /* Room left: write in place (no realloc). Out of room: double the
       capacity, moving the elements (bitwise — ownership transfers)
       and freeing the old buffer. Doubling off cap 0 seeds 4. */
    LLVMValueRef hasRoom = LLVMBuildICmp(b->m_builder, LLVMIntULT, oldLen, oldCap, "room");
    LLVMBasicBlockRef roomBb = NewBb(b, "push.room");
    LLVMBasicBlockRef growBb = NewBb(b, "push.grow");
    LLVMBasicBlockRef joinBb = NewBb(b, "push.join");
    LLVMBuildCondBr(b->m_builder, hasRoom, roomBb, growBb);
    b->m_terminated = true;

    PositionAtEnd(b, roomBb);
    Br(b, joinBb);

    PositionAtEnd(b, growBb);
    LLVMValueRef doubled = LLVMBuildMul(b->m_builder, oldCap, LLVMConstInt(I32Ty(b), 2, 0), "capx2");
    LLVMValueRef newCap = LLVMBuildSelect(
        b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, oldCap, LLVMConstInt(I32Ty(b), 0, 0), "capz"),
        LLVMConstInt(I32Ty(b), 4, 0), doubled, "newcap");
    LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), WidenLen(b, newCap), "bytes");
    LLVMValueRef allocArgs[1] = {totalBytes};
    StrataAllocFn(b);
    LLVMValueRef grown = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "pushbuf");

    EmitArrayCopyLoop(b, grown, oldData, oldLen64, elemTy);

    LLVMValueRef freeArgs[1] = {oldData};
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

    /* The copy loop leaves the builder in its own `end` block — that
       block (not push.grow) is the direct predecessor of the join. */
    LLVMBasicBlockRef growEnd = LLVMGetInsertBlock(b->m_builder);
    Br(b, joinBb);

    PositionAtEnd(b, joinBb);
    LLVMValueRef data = LLVMBuildPhi(b->m_builder, b->m_ptrTy, "pushdata");
    LLVMValueRef capPhi = LLVMBuildPhi(b->m_builder, I32Ty(b), "pushcap");
    LLVMValueRef dataIn[2] = {oldData, grown};
    LLVMValueRef capIn[2] = {oldCap, newCap};
    LLVMBasicBlockRef fromBbs[2] = {roomBb, growEnd};
    LLVMAddIncoming(data, dataIn, fromBbs, 2);
    LLVMAddIncoming(capPhi, capIn, fromBbs, 2);

    LLVMValueRef slotIdx[1] = {oldLen64};
    LLVMValueRef elAddr = LLVMBuildGEP2(b->m_builder, elemTy, data, slotIdx, 1, "pushslot");

    StoreMovedElement(b, elAddr, v, valNode, elemTd);

    LLVMValueRef newLen = LLVMBuildAdd(b->m_builder, oldLen, one32, "nlen");
    LLVMBuildStore(b->m_builder, data, dataPtrPtr);
    LLVMBuildStore(b->m_builder, capPhi, capPtr);
    LLVMBuildStore(b->m_builder, newLen, lenPtr);

    return ValueMake(newLen, uintTd);
}

/* array_resize: within cap it mutates in place (zero the grown tail /
   drop the shrunken owning tail); beyond cap it reallocates to
   max(newLen, cap*2, 4) and moves the prefix. */
Value EmitArrayResize(Builder* b, CallExpr* n, const TypeName* arrayInner, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr,
                      LLVMValueRef capPtr, TypeDesc elemTd, LLVMTypeRef elemTy)
{
    /* New length first, for the same reason as array_push: evaluating it
       may mutate the array. */
    LLVMValueRef newLen = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    LLVMValueRef oldLen = LLVMBuildLoad2(b->m_builder, I32Ty(b), lenPtr, "len");
    LLVMValueRef oldLen64 = WidenLen(b, oldLen);
    LLVMValueRef oldData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
    LLVMValueRef oldCap64 = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), capPtr, "cap"));

    LLVMValueRef fits = LLVMBuildICmp(b->m_builder, LLVMIntULE, newLen, oldCap64, "fits");
    LLVMBasicBlockRef fitBb = NewBb(b, "rsz.fit");
    LLVMBasicBlockRef reallocBb = NewBb(b, "rsz.realloc");
    LLVMBasicBlockRef doneBb = NewBb(b, "rsz.done");
    LLVMBuildCondBr(b->m_builder, fits, fitBb, reallocBb);
    b->m_terminated = true;

    PositionAtEnd(b, fitBb);
    {
        LLVMValueRef grows = LLVMBuildICmp(b->m_builder, LLVMIntULT, oldLen64, newLen, "grows");
        LLVMBasicBlockRef zeroBb = NewBb(b, "rsz.zero");
        LLVMBasicBlockRef dropBb = NewBb(b, "rsz.drop");
        LLVMBasicBlockRef fitEnd = NewBb(b, "rsz.fitend");
        LLVMBuildCondBr(b->m_builder, grows, zeroBb, dropBb);
        b->m_terminated = true;

        PositionAtEnd(b, zeroBb);
        EmitArrayZeroLoop(b, oldData, oldLen64, newLen, elemTy);
        Br(b, fitEnd);

        PositionAtEnd(b, dropBb);

        if (arrayInner && BuilderIsOwningValue(b, arrayInner))
        {
            EmitArrayDropRange(b, oldData, newLen, oldLen64, elemTd, elemTy);
        }

        Br(b, fitEnd);

        PositionAtEnd(b, fitEnd);
        LLVMBuildStore(b->m_builder, LLVMBuildTrunc(b->m_builder, newLen, I32Ty(b), "nlen32"), lenPtr);
        Br(b, doneBb);
    }

    PositionAtEnd(b, reallocBb);
    {
        LLVMValueRef capx2 = LLVMBuildMul(b->m_builder, oldCap64, LLVMConstInt(I64Ty(b), 2, 0), "capx2");
        LLVMValueRef atLeastN = LLVMBuildSelect(
            b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntULT, capx2, newLen, "cx2lt"), newLen, capx2, "mc1");
        LLVMValueRef newCap = LLVMBuildSelect(
            b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntULT, atLeastN, LLVMConstInt(I64Ty(b), 4, 0), "mc1lt"),
            LLVMConstInt(I64Ty(b), 4, 0), atLeastN, "newcap");

        LLVMValueRef allocBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), newCap, "bytes");
        LLVMValueRef allocArgs[1] = {allocBytes};
        StrataAllocFn(b);
        LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "rszbuf");

        LLVMValueRef cmpLt = LLVMBuildICmp(b->m_builder, LLVMIntULT, oldLen64, newLen, "lt");
        LLVMValueRef copyCount = LLVMBuildSelect(b->m_builder, cmpLt, oldLen64, newLen, "cc");

        EmitArrayCopyLoop(b, newData, oldData, copyCount, elemTy);
        EmitArrayZeroLoop(b, newData, copyCount, newLen, elemTy);

        LLVMValueRef freeArgs[1] = {oldData};
        StrataFreeFn(b);
        LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

        LLVMBuildStore(b->m_builder, newData, dataPtrPtr);
        LLVMBuildStore(b->m_builder, LLVMBuildTrunc(b->m_builder, newCap, I32Ty(b), "nc32"), capPtr);
        LLVMBuildStore(b->m_builder, LLVMBuildTrunc(b->m_builder, newLen, I32Ty(b), "nl32"), lenPtr);
        Br(b, doneBb);
    }

    PositionAtEnd(b, doneBb);
    return ValueMake(NULL, TypeDescMake(NULL, TDF_VOID, NULL));
}

/* Copies a non-empty fat array value: fresh buffer + per-element deep copy
   for owning element types, memcpy-style loop otherwise. Callers guard
   empty optionals. */
Value EmitArrayValueCopy(Builder* b, Value src, TypeDesc td, TypeDesc elemTd, LLVMTypeRef elemTy, LLVMValueRef oldLen,
                         LLVMValueRef oldData)
{
    oldLen = WidenLen(b, oldLen); /* the fat's u32 length, as a loop bound */
    LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), oldLen, "cby");
    LLVMValueRef allocArgs[1] = {totalBytes};
    StrataAllocFn(b);
    LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "cpya");

    if (BuilderIsOwningValue(b, td.arrayInner))
    {
        LLVMBasicBlockRef cond = NewBb(b, "ccp.cond");
        LLVMBasicBlockRef body = NewBb(b, "ccp.body");
        LLVMBasicBlockRef end = NewBb(b, "ccp.end");
        LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "ccp.i");
        LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
        Br(b, cond);

        PositionAtEnd(b, cond);
        LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
        LLVMValueRef cnt = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, oldLen, "cclt");
        LLVMBuildCondBr(b->m_builder, cnt, body, end);
        b->m_terminated = true;

        PositionAtEnd(b, body);
        LLVMValueRef ep[1] = {i};
        LLVMValueRef srcElem = LLVMBuildGEP2(b->m_builder, elemTy, oldData, ep, 1, "cse");
        LLVMValueRef dstElem = LLVMBuildGEP2(b->m_builder, elemTy, newData, ep, 1, "cde");
        Value elemVal = ValueMake(LLVMBuildLoad2(b->m_builder, elemTy, srcElem, "ev"), elemTd);
        Value copied = EmitCopyValue(b, elemVal, elemTd);
        LLVMBuildStore(b->m_builder, copied.value, dstElem);
        LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
        LLVMBuildStore(b->m_builder, next, iSlot);
        Br(b, cond);

        PositionAtEnd(b, end);
    }
    else
    {
        EmitArrayCopyLoop(b, newData, oldData, oldLen, elemTy);
    }

    LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
    arr = LLVMBuildInsertValue(b->m_builder, arr, newData, 0, "ci");
    arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMBuildTrunc(b->m_builder, oldLen, I32Ty(b), "cl32"), 1, "cl");
    arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMBuildTrunc(b->m_builder, oldLen, I32Ty(b), "cc32"), 2, "cc");
    return ValueMake(arr, td);
}

/* The backing buffer for a literal's `count` elements: a fixed stack array in
   the entry block when `stackBuffer`, else a heap allocation. */
LLVMValueRef EmitArrayLiteralBuffer(Builder* b, LLVMTypeRef elemTy, LLVMTypeRef slotTy, size_t count, bool stackBuffer)
{
    LLVMValueRef dataPtr = LLVMConstNull(b->m_ptrTy);

    if (stackBuffer)
    {
        /* Fixed-size stack buffer (count is a compile-time constant). */
        size_t n = count;
        LLVMTypeRef arrTy = LLVMArrayType(slotTy, (unsigned)n);
        LLVMValueRef bufSlot = EntryAlloca(b, arrTy, "varargs");

        LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);
        LLVMValueRef idx[2] = {zero, zero};
        dataPtr = LLVMBuildGEP2(b->m_builder, arrTy, bufSlot, idx, 2, "varargsdata");
    }
    else
    {
        LLVMValueRef elemSize = SizeOfConst(b, elemTy);
        LLVMValueRef countConst = LLVMConstInt(I64Ty(b), (unsigned long long)count, 0);
        LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, elemSize, countConst, "bytes");
        LLVMValueRef allocArgs[1] = {totalBytes};
        StrataAllocFn(b);
        dataPtr = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "arrbuf");
    }

    return dataPtr;
}

/* A `ref T... rest` element with non-owning elements: the slot holds a POINTER
   to the source argument, so element writes reach the caller. */
void EmitAliasedArrayElement(Builder* b, Node* eNode, Value v, LLVMValueRef elemAddr, TypeDesc elemTd,
                             LLVMTypeRef elemTy)
{
    /* Store a POINTER to the source argument. A box arg already
       is the value's address; lvalues yield their address; a
       non-lvalue is materialized into a stack temp. */
    if (TypeDescIsBox(&v.typeDesc))
    {
        LLVMBuildStore(b->m_builder, v.value, elemAddr);
    }
    else
    {
        LValue lv = EmitLValue(b, eNode);

        if (lv.valid)
        {
            LLVMBuildStore(b->m_builder, lv.ptr, elemAddr);
        }
        else
        {
            LLVMValueRef temp = EntryAlloca(b, elemTy, "aliastemp");
            LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, temp);
            LLVMBuildStore(b->m_builder, temp, elemAddr);
        }
    }
}

/* Stores one element of a literal by its element type (string, array, ^T,
   owning struct or a plain value). Movable sources are nulled unless the rest
   is borrowed. */
void EmitArrayElementStore(Builder* b, Node* eNode, Value v, LLVMValueRef elemAddr, TypeDesc elemTd, bool borrow)
{
    if (TypeDescIsString(&elemTd))
    {
        /* string element: literals are heap-copied (the constant is
           borrowed; the array owns its elements); variables move. */
        if (borrow)
        {
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
        }
        else if (eNode->kind == NodeStrLiteral)
        {
            LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, v.value, 0, "aelit.p");
            LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, v.value, 1, "aelit.l");
            LLVMBuildStore(b->m_builder, BuildOwnedStringFat(b, sp, sl), elemAddr);
        }
        else
        {
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
            NullMovedSource(b, eNode);
        }
    }
    else if (TypeDescHasArrayShape(&elemTd))
    {
        /* Array element: `v` is already the evaluated child (a nested
           literal was built by the EmitExpr above - building it again
           here leaked the first copy). A movable source (`{ xs }`)
           moves in, like the string and box branches. */
        LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elemAddr);

        if (!borrow)
        {
            NullMovedSource(b, eNode);
        }
    }
    else if (TypeDescIsBox(&elemTd) && elemTd.boxInner)
    {
        /* ^T element. */
        bool sameOwningType = TypeDescIsBox(&v.typeDesc) && v.typeDesc.boxInner
                              && strcmp(v.typeDesc.boxInner->name, elemTd.boxInner->name) == 0;

        if (borrow)
        {
            /* ref rest: borrow, keep source alive. */
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
        }
        else if (sameOwningType)
        {
            /* Same ^T type: move. */
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
            NullMovedSource(b, eNode);
        }
        else
        {
            /* ^T from a non-^T value: box it up. */
            LLVMValueRef heap = EmitBoxCell(b, elemTd.boxInner, v, eNode, "abox");
            LLVMBuildStore(b->m_builder, heap, elemAddr);
        }
    }
    else if (TypeDescIsBox(&elemTd))
    {
        /* string element. */
        if (borrow)
        {
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
        }
        else
        {
            LLVMValueRef owned = EmitOwnedValue(b, v, eNode, StringTypeName(b));
            LLVMBuildStore(b->m_builder, owned, elemAddr);
        }
    }
    else if (!borrow && BoxIntoInlineOwning(b, elemTd, v.typeDesc))
    {
        LLVMBuildStore(b->m_builder, TakeBoxContents(b, v, eNode).value, elemAddr);
    }
    else if (elemTd.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, elemTd.structTypeName))
    {
        /* Owning-struct element (`Rec[]` literal): temporaries (struct
            literals, call results) transfer bitwise; movable sources
            move and are zeroed. */
        if (borrow)
        {
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
        }
        else
        {
            LLVMBuildStore(b->m_builder, v.value, elemAddr);
            NullMovedSource(b, eNode);
        }
    }
    else
    {
        LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elemAddr);
    }
}

/* Builds a T[] from element exprs, shared by array literals (heap-backed) and
   typed rest params. rest=stack-backed (alloca in entry); borrow=true (ref
   rest) stores owning element pointers without nulling/moving sources. */
Value EmitArrayFromNodes(Builder* b, const TypeName* elementType, const Vec* elements, bool stackBuffer, bool borrow)
{
    TypeDesc elemTd = Resolve(b, elementType);

    /* The LLVM type of one element slot in the backing buffer. Arrays and
        boxes/strings both reduce to a pointer; structs/scalars use their
        own type. */
    LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

    /* A `ref T... rest` with non-owning elements holds POINTERS to the source
        arguments (aliased), so element writes propagate to the caller. */
    bool aliased = stackBuffer && borrow && !BuilderIsOwningType(b, elementType);
    LLVMTypeRef slotTy = aliased ? b->m_ptrTy : elemTy;

    size_t count = elements->count;
    LLVMValueRef dataPtr = LLVMConstNull(b->m_ptrTy);

    if (count > 0)
    {
        dataPtr = EmitArrayLiteralBuffer(b, elemTy, slotTy, count, stackBuffer);

        for (size_t i = 0; i < count; i++)
        {
            Node* eNode = (Node*)VecGet(elements, i);
            Value v = EmitExpr(b, eNode);

            LLVMValueRef idxArg[1] = {LLVMConstInt(I64Ty(b), (unsigned long long)i, 0)};
            LLVMValueRef elemAddr = LLVMBuildGEP2(b->m_builder, slotTy, dataPtr, idxArg, 1, "ael");

            if (aliased)
            {
                EmitAliasedArrayElement(b, eNode, v, elemAddr, elemTd, elemTy);
                continue;
            }

            EmitArrayElementStore(b, eNode, v, elemAddr, elemTd, borrow);
        }
    }

    LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
    arr = LLVMBuildInsertValue(b->m_builder, arr, dataPtr, 0, "arri");
    arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I32Ty(b), (unsigned long long)count, 0), 1, "arrl");
    arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I32Ty(b), (unsigned long long)count, 0), 2, "arrc");

    TypeDesc td = {0};
    td.type = ArrayStructType(b);
    TypeDescSetArrayShape(&td, true);
    td.arrayInner = elementType;
    TypeDescSetAliasedArray(&td, aliased);

    return ValueMake(arr, td);
}

/* True for `{}` once typed: an empty braced value, which default-constructs. */
static bool IsEmptyBracedNode(const Node* n)
{
    return (n->kind == NodeArrayInit && ((const ArrayInitExpr*)n)->elements.count == 0
            && !((const ArrayInitExpr*)n)->repeatCount)
           || (n->kind == NodeStructInit && ((const StructInitExpr*)n)->fields.count == 0);
}

/* `{ value; count }` for a dynamic array: `count` elements, each a copy of the value (evaluated
   once), or each zero for a `{}` value. A variable used as the value is only read;
   a temporary is built once, copied, and dropped. */
static Value EmitArrayRepeat(Builder* b, ArrayInitExpr* n)
{
    TypeDesc elemTd = Resolve(b, n->elementType);
    LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;
    bool owning = BuilderIsOwningValue(b, n->elementType);
    Node* valueNode = (Node*)VecGet(&n->elements, 0);
    bool fresh = IsEmptyBracedNode(valueNode);
    bool borrowed = !fresh && MovableBoxSourceNode(valueNode) != NULL;

    LLVMValueRef count = AsI64Index(b, EmitExpr(b, n->repeatCount));

    /* A negative count would size the buffer as a huge unsigned one. */
    EmitPanicIf(b, LLVMBuildICmp(b->m_builder, LLVMIntSLT, count, LLVMConstInt(I64Ty(b), 0, 0), "fill.neg"),
                "array fill count is negative");

    /* The value, once: read in place, or owned by a one-element temporary array. */
    Value source = {0};
    LLVMValueRef tempData = NULL;

    if (!fresh)
    {
        if (borrowed || !owning)
        {
            source = EmitExpr(b, valueNode);

            /* `{ boxed; n }` for a bare element type copies what the box points at, not the box pointer. */
            if (TypeDescIsBox(&source.typeDesc) && !TypeDescIsBox(&elemTd) && !TypeDescHasArrayShape(&elemTd)
                && !TypeDescIsString(&elemTd))
            {
                source = DerefBoxValue(b, source);
            }

            source = owning ? source : Coerce(b, source, elemTd);
        }
        else
        {
            Value one = EmitArrayFromNodes(b, n->elementType, &n->elements, false, false);
            tempData = LLVMBuildExtractValue(b->m_builder, one.value, 0, "rep.tmp");
            source = ValueMake(LLVMBuildLoad2(b->m_builder, elemTy, tempData, "rep.v"), elemTd);
        }
    }

    /* At least one byte, so the buffer is never null for a non-empty fat. */
    LLVMValueRef bytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), count, "rep.bytes");
    LLVMValueRef isZero = LLVMBuildICmp(b->m_builder, LLVMIntEQ, bytes, LLVMConstInt(I64Ty(b), 0, 0), "rep.z");
    bytes = LLVMBuildSelect(b->m_builder, isZero, LLVMConstInt(I64Ty(b), 1, 0), bytes, "rep.alloc");
    LLVMValueRef allocArgs[1] = {bytes};
    StrataAllocFn(b);
    LLVMValueRef data = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "rep.buf");

    LLVMBasicBlockRef cond = NewBb(b, "rep.cond");
    LLVMBasicBlockRef body = NewBb(b, "rep.body");
    LLVMBasicBlockRef end = NewBb(b, "rep.end");
    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "rep.i");
    LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntULT, i, count, "rep.lt"), body, end);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = {i};
    LLVMValueRef slot = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "rep.slot");

    if (fresh)
    {
        /* The default value: empty strings, arrays and optionals, zeroed scalars (sema checked that is
           valid), with field defaults applied, so a Drop type's elements are alive and get dropped. */
        bool plainStruct = elemTd.structTypeName && !TypeDescIsBox(&elemTd) && !TypeDescHasArrayShape(&elemTd);
        LLVMValueRef zero = plainStruct ? DefaultValueOf(b, n->elementType, elemTd) : LLVMConstNull(elemTy);
        LLVMBuildStore(b->m_builder, zero, slot);
    }
    else if (owning)
    {
        LLVMBuildStore(b->m_builder, EmitCopyValue(b, source, elemTd).value, slot);
    }
    else
    {
        LLVMBuildStore(b->m_builder, source.value, slot);
    }

    LLVMBuildStore(b->m_builder, LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "rep.next"), iSlot);
    Br(b, cond);

    PositionAtEnd(b, end);

    if (tempData)
    {
        EmitArrayDropRange(b, tempData, LLVMConstInt(I64Ty(b), 0, 0), LLVMConstInt(I64Ty(b), 1, 0), elemTd, elemTy);
        LLVMValueRef freeArgs[1] = {tempData};
        StrataFreeFn(b);
        LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");
    }

    LLVMValueRef count32 = LLVMBuildTrunc(b->m_builder, count, I32Ty(b), "rep.n");
    LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
    arr = LLVMBuildInsertValue(b->m_builder, arr, data, 0, "arri");
    arr = LLVMBuildInsertValue(b->m_builder, arr, count32, 1, "arrl");
    arr = LLVMBuildInsertValue(b->m_builder, arr, count32, 2, "arrc");

    TypeDesc td = {0};
    td.type = ArrayStructType(b);
    TypeDescSetArrayShape(&td, true);
    td.arrayInner = n->elementType;

    return ValueMake(arr, td);
}

Value EmitArrayInit(Builder* b, ArrayInitExpr* n)
{
    if (n->repeatCount && n->elementType)
    {
        return EmitArrayRepeat(b, n);
    }

    /* An untyped non-empty literal means no context ever supplied an
        element type (a gap in inference) - diagnose instead of crashing
        on a NULL type. */
    if (!n->elementType && n->elements.count > 0)
    {
        if (b->m_diag)
        {
            DiagError(b->m_diag, n->base.range, "array literal element type could not be inferred");
        }

        n->elements.count = 0;
    }

    /* An empty braced literal with no contextual element type (`FooBar("x", {})`)
        is the canonical empty array - nothing to resolve or allocate. */
    if (!n->elementType && n->elements.count == 0)
    {
        LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
        arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstNull(b->m_ptrTy), 0, "arri");
        arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I32Ty(b), 0, 0), 1, "arrl");
        arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I32Ty(b), 0, 0), 2, "arrc");

        TypeDesc td = {0};
        td.type = ArrayStructType(b);
        TypeDescSetArrayShape(&td, true);
        return ValueMake(arr, td);
    }

    return EmitArrayFromNodes(b, n->elementType, &n->elements, false, false);
}

/* Product of the fixed dimensions at/below `t` (1 once a non-fixed node is
   reached). Used to map a flat literal index onto nested array dimensions. */
ArrayLength FixedDimProduct(const TypeName* t)
{
    ArrayLength count = 1;

    while (t && TypeNameHasArrayShape(t) && t->length >= 0)
    {
        count *= t->length;
        t = t->elem;
    }

    return count;
}

/* Inserts element `k` (a FLAT index, C-initialized-array style) of a
   (possibly nested) fixed array value, descending through inner fixed
   dimensions with extract/insert pairs. */
LLVMValueRef InsertFixedElem(Builder* b, LLVMValueRef agg, LLVMValueRef val, const TypeName* arrType, ArrayLength k)
{
    if (!(TypeNameHasArrayShape(arrType) && arrType->length >= 0))
    {
        return agg;
    }

    if (arrType->elem && TypeNameHasArrayShape(arrType->elem) && arrType->elem->length >= 0)
    {
        ArrayLength innerCount = FixedDimProduct(arrType->elem);
        ArrayLength outer = innerCount > 0 ? k / innerCount : 0;
        ArrayLength inner = innerCount > 0 ? k % innerCount : 0;

        LLVMValueRef sub = LLVMBuildExtractValue(b->m_builder, agg, (FieldIndex)outer, "sub");
        sub = InsertFixedElem(b, sub, val, arrType->elem, inner);
        return LLVMBuildInsertValue(b->m_builder, agg, sub, (FieldIndex)outer, "ins");
    }

    return LLVMBuildInsertValue(b->m_builder, agg, val, (FieldIndex)k, "ins");
}

void EmitFixedArrayVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc)
{
    /* Stack-allocated fixed-size array local */
    LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "fixarr");
    LLVMValueRef arr = DefaultValueOf(b, &varDecl->type, typeDesc);

    if (varDecl->init && varDecl->init->kind == NodeArrayInit)
    {
        ArrayInitExpr* ai = AsNode(ArrayInitExpr, varDecl->init);

        /* Sema guarantees the leaf element type; fall back to a
           leaf-walk of the declared type if it is missing or not a
           leaf (a non-leaf would miscompile inserts as int<->array
           casts). */
        const TypeName* leafTn = ai->elementType;

        if (!leafTn || (TypeNameHasArrayShape(leafTn) && leafTn->length >= 0))
        {
            leafTn = &varDecl->type;

            while (leafTn && TypeNameHasArrayShape(leafTn) && leafTn->length >= 0)
            {
                leafTn = leafTn->elem;
            }
        }

        TypeDesc elemTd = leafTn ? Resolve(b, leafTn) : Resolve(b, typeDesc.arrayInner);

        if (ai->repeatCount)
        {
            /* `{ value; N }`: the value once, in every element. */
            Value ev = Coerce(b, EmitExpr(b, (Node*)VecGet(&ai->elements, 0)), elemTd);
            ArrayLength total = FixedDimProduct(&varDecl->type);

            for (ArrayLength k = 0; k < total; k++)
            {
                arr = InsertFixedElem(b, arr, ev.value, &varDecl->type, k);
            }
        }

        for (size_t k = 0; k < ai->elements.count && !ai->repeatCount; k++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, k);

            if (!elem)
            {
                continue;
            }

            Value ev = Coerce(b, EmitExpr(b, elem), elemTd);
            arr = InsertFixedElem(b, arr, ev.value, &varDecl->type, (ArrayLength)k);
        }
    }

    LLVMBuildStore(b->m_builder, arr, slot);

    Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
    sym->value = slot;
    sym->typeDesc = typeDesc;

    SymDeclare(b, varDecl->name, sym);
}

void EmitArrayVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc)
{
    LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "arr");

    if (varDecl->init)
    {
        if (varDecl->init->kind == NodeArrayInit)
        {
            LLVMValueRef arr = EmitArrayInit(b, AsNode(ArrayInitExpr, varDecl->init)).value;
            LLVMBuildStore(b->m_builder, arr, slot);
        }
        else if (TypeDescIsString(&typeDesc) && varDecl->init->kind == NodeStrLiteral)
        {
            /* string from a literal: heap-copy (the constant is
               borrowed; the binding owns its bytes). */
            Value value = EmitExpr(b, varDecl->init);
            LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, value.value, 0, "vslit.p");
            LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, value.value, 1, "vslit.l");
            LLVMValueRef owned = BuildOwnedStringFat(b, sp, sl);
            LLVMBuildStore(b->m_builder, owned, slot);
        }
        else
        {
            /* Move from another array/string binding: copy the
               {ptr,len} struct and zero the source (whole slot) to
               avoid a double free. */
            Node* movedNode = MovedSource(varDecl->init);
            LValue src = movedNode ? EmitLValueForNullStore(b, movedNode) : (LValue){0};

            Value value = EmitExpr(b, varDecl->init);
            LLVMBuildStore(b->m_builder, Coerce(b, value, typeDesc).value, slot);

            /* A piece of a temporary only exists once evaluated. */
            if (!src.valid && movedNode && FindTempPart(b, movedNode))
            {
                src = EmitLValueForNullStore(b, movedNode);
            }

            if (src.valid && TypeDescHasArrayShape(&src.typeDesc))
            {
                LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
            }
        }
    }
    else
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), slot);
    }

    Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
    sym->value = slot;
    sym->typeDesc = typeDesc;

    SymDeclare(b, varDecl->name, sym);

    OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
    ol->slot = slot;
    ol->td = typeDesc;
    VecPush(&b->m_owningLocals, ol);
}
