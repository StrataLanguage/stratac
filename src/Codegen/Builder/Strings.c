// Builder: String and cstring values: fat strings, concatenation, copies, equality helpers, text casts.
#include "Codegen/Builder/BuilderInternal.h"

// Emits a string literal as a `.rodata` constant and returns the `i8*`
// pointer to its first byte (with NUL terminator). Shared by every
// `cstring` literal use site.
LLVMValueRef EmitCStringLiteralPtr(Builder* b, const StrLiteral* literal)
{
    size_t len = strlen(literal->value);
    LLVMValueRef strConst = LLVMConstStringInContext(b->ctx, literal->value, (unsigned)len, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);

    char* gName = ArenaFormat(b->arena, ".cstr.%d", b->strLitCount++);
    LLVMValueRef global = LLVMAddGlobal(b->mod, strType, gName);
    LLVMSetInitializer(global, strConst);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(global, 1);
    LLVMSetGlobalConstant(global, 1);

    LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
    LLVMValueRef idx[2] = {zero, zero};
    return LLVMConstGEP2(strType, global, idx, 2);
}

// Assembles a fat string value from a data pointer and a length (i32 or
// i64; widened/truncated as needed). cap = len+1: the buffer's NUL slot
// counts as capacity.
static LLVMValueRef MakeStringFatValue(Builder* b, LLVMValueRef dataPtr, LLVMValueRef lenVal)
{
    LLVMValueRef len = WidenLen(b, lenVal);
    LLVMValueRef cap = LLVMBuildAdd(b->builder, len, LLVMConstInt(I64Ty(b), 1, 0), "sfat.c");
    LLVMValueRef fat = LLVMGetUndef(ArrayStructType(b));
    fat = LLVMBuildInsertValue(b->builder, fat, dataPtr, 0, "sfat.p");
    fat = LLVMBuildInsertValue(b->builder, fat, LLVMBuildTrunc(b->builder, len, I32Ty(b), "sfat.l32"), 1, "sfat.l");
    return LLVMBuildInsertValue(b->builder, fat, LLVMBuildTrunc(b->builder, cap, I32Ty(b), "sfat.c32"), 2, "sfat.c");
}

// The TypeDesc of a `string` value: fat {ptr, len, cap}, like T[].
TypeDesc StringFatDesc(Builder* b)
{
    TypeDesc td = {0};
    td.type = ArrayStructType(b);
    TypeDescSetArrayShape(&td, true);
    TypeDescSetString(&td, true);
    return td;
}

// Builds an OWNED fat string value from srcPtr[0..len): allocates len+1
// bytes, copies, and writes the NUL terminator at [len] — the invariant
// the extern-boundary pun (fat -> char*) relies on.
LLVMValueRef BuildOwnedStringFat(Builder* b, LLVMValueRef srcPtr, LLVMValueRef lenVal)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
    lenVal = WidenLen(b, lenVal); // callers may hand a u32 fat length
    LLVMValueRef copyBytes = LLVMBuildAdd(b->builder, lenVal, one, "slen");
    LLVMValueRef allocArgs[1] = {copyBytes};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, allocArgs, 1, "sbuf");

    LLVMBasicBlockRef cond = NewBb(b, "scpy.cond");
    LLVMBasicBlockRef body = NewBb(b, "scpy.body");
    LLVMBasicBlockRef done = NewBb(b, "scpy.done");
    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "scpy.i");
    LLVMBuildStore(b->builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "i");
    LLVMValueRef more = LLVMBuildICmp(b->builder, LLVMIntULT, i, lenVal, "slt");
    LLVMBuildCondBr(b->builder, more, body, done);
    b->terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef srcByte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, srcPtr, &i, 1, "sb"), "b");
    LLVMBuildStore(b->builder, srcByte, LLVMBuildGEP2(b->builder, i8Ty, heap, &i, 1, "db"));
    LLVMValueRef next = LLVMBuildAdd(b->builder, i, one, "next");
    LLVMBuildStore(b->builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, done);
    // NUL terminator at [len].
    LLVMValueRef termIdx[1] = {lenVal};
    LLVMBuildStore(b->builder, LLVMConstNull(i8Ty), LLVMBuildGEP2(b->builder, i8Ty, heap, termIdx, 1, "sterm"));

    return MakeStringFatValue(b, heap, lenVal);
}

// dst[i] = src[i] for i in [0, len): an i64 byte loop.
static void EmitByteCopy(Builder* b, LLVMValueRef dst, LLVMValueRef src, LLVMValueRef len)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
    LLVMBasicBlockRef cond = NewBb(b, "bcpy.cond");
    LLVMBasicBlockRef body = NewBb(b, "bcpy.body");
    LLVMBasicBlockRef done = NewBb(b, "bcpy.done");
    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "bcpy.i");
    LLVMBuildStore(b->builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "i");
    LLVMValueRef more = LLVMBuildICmp(b->builder, LLVMIntULT, i, len, "blt");
    LLVMBuildCondBr(b->builder, more, body, done);
    b->terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef byte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, src, &i, 1, "sb"), "b");
    LLVMBuildStore(b->builder, byte, LLVMBuildGEP2(b->builder, i8Ty, dst, &i, 1, "db"));
    LLVMBuildStore(b->builder, LLVMBuildAdd(b->builder, i, one, "next"), iSlot);
    Br(b, cond);

    PositionAtEnd(b, done);
}

// A new owned string holding a[0..alen) then b[0..blen), NUL-terminated.
LLVMValueRef BuildConcatStringFat(Builder* b, LLVMValueRef aPtr, LLVMValueRef aLen, LLVMValueRef bPtr,
                                  LLVMValueRef bLen)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef total = LLVMBuildAdd(b->builder, aLen, bLen, "cat.len");
    LLVMValueRef allocArgs[1] = {LLVMBuildAdd(b->builder, total, LLVMConstInt(I64Ty(b), 1, 0), "cat.bytes")};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, allocArgs, 1, "cat.buf");

    EmitByteCopy(b, heap, aPtr, aLen);
    EmitByteCopy(b, LLVMBuildGEP2(b->builder, i8Ty, heap, &aLen, 1, "cat.mid"), bPtr, bLen);

    LLVMBuildStore(b->builder, LLVMConstNull(i8Ty), LLVMBuildGEP2(b->builder, i8Ty, heap, &total, 1, "cat.term"));

    return MakeStringFatValue(b, heap, total);
}

// Computes the NUL-terminated length of `s` (an i64 byte loop).
LLVMValueRef BuildStrLen(Builder* b, LLVMValueRef s)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "cstrlen.i");
    LLVMBuildStore(b->builder, zero, iSlot);

    LLVMBasicBlockRef cond = NewBb(b, "cstrlen.cond");
    LLVMBasicBlockRef body = NewBb(b, "cstrlen.body");
    LLVMBasicBlockRef done = NewBb(b, "cstrlen.done");
    LLVMBasicBlockRef nullBB = NewBb(b, "cstrlen.null");

    // A NULL pointer has length 0 (defensive: a zero-initialized `cstring`
    // struct field or omitted field init is NULL, never a valid buffer).
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, s, LLVMConstNull(b->ptrTy), "cstrlen.null");
    LLVMBuildCondBr(b->builder, isNull, nullBB, cond);
    b->terminated = true;

    PositionAtEnd(b, nullBB);
    Br(b, done);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "cstrlen.i");
    LLVMValueRef c
        = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, s, &i, 1, "cstrlen.p"), "cstrlen.c");
    LLVMValueRef isNul = LLVMBuildICmp(b->builder, LLVMIntEQ, c, LLVMConstNull(i8Ty), "cstrlen.nul");
    LLVMBuildCondBr(b->builder, isNul, done, body);
    b->terminated = true;

    PositionAtEnd(b, body);
    LLVMBuildStore(b->builder, LLVMBuildAdd(b->builder, i, one, "cstrlen.n"), iSlot);
    Br(b, cond);

    PositionAtEnd(b, done);
    return LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "cstrlen.len");
}

// Converts a host-owned NUL-terminated char* (an extern `string` return) into
// an OWNED fat string: allocates len+1 bytes, copies, NUL-terminates. A NULL
// pointer yields the canonical empty {null, 0, 0}.
LLVMValueRef BuildOwnedStringFromCStr(Builder* b, LLVMValueRef cstr)
{
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, cstr, LLVMConstNull(b->ptrTy), "cstr.null");

    LLVMBasicBlockRef nullBB = NewBb(b, "cstr.null");
    LLVMBasicBlockRef copyBB = NewBb(b, "cstr.copy");
    LLVMBasicBlockRef mergeBB = NewBb(b, "cstr.merge");
    LLVMBuildCondBr(b->builder, isNull, nullBB, copyBB);

    PositionAtEnd(b, nullBB);
    LLVMValueRef emptyFat = LLVMConstNull(ArrayStructType(b));
    Br(b, mergeBB);

    PositionAtEnd(b, copyBB);
    LLVMValueRef len = BuildStrLen(b, cstr);
    LLVMValueRef owned = BuildOwnedStringFat(b, cstr, len);
    LLVMBasicBlockRef doneBB = LLVMGetInsertBlock(b->builder);
    Br(b, mergeBB);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->builder, ArrayStructType(b), "cstr.fat");
    LLVMValueRef incoming[2] = {emptyFat, owned};
    LLVMBasicBlockRef blocks[2] = {nullBB, doneBB};
    LLVMAddIncoming(phi, incoming, blocks, 2);
    return phi;
}

// Cached static NUL byte: a valid empty C string for the extern pun.
LLVMValueRef EmptyNulString(Builder* b)
{
    if (!b->emptyNul)
    {
        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
        LLVMTypeRef arrTy = LLVMArrayType(i8Ty, 1);
        LLVMValueRef global = LLVMAddGlobal(b->mod, arrTy, ".emptystr");
        LLVMSetInitializer(global, LLVMConstNull(arrTy));
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(global, 1);
        LLVMSetGlobalConstant(global, 1);

        LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
        LLVMValueRef idx[2] = {zero, zero};
        b->emptyNul = LLVMConstGEP2(arrTy, global, idx, 2);
    }

    return b->emptyNul;
}

// Extracts the char* the C ABI sees for a string value: the data pointer,
// with a valid empty NUL-terminated buffer substituted when the string is
// the canonical empty {null, 0} (passing NULL to C string APIs is UB).
LLVMValueRef BuildExternStringPtr(Builder* b, Value v)
{
    LLVMValueRef dataPtr = LLVMBuildExtractValue(b->builder, v.value, 0, "sptr");
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, dataPtr, LLVMConstNull(b->ptrTy), "snull");

    return LLVMBuildSelect(b->builder, isNull, EmptyNulString(b), dataPtr, "sextern");
}

LLVMValueRef EmptyStrConstant(Builder* b)
{
    char nul[1] = {0};
    LLVMValueRef strConst = LLVMConstStringInContext(b->ctx, nul, 1, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);
    char* gName = ArenaFormat(b->arena, ".oob.empty.%d", b->strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->mod, strType, gName);
    LLVMSetInitializer(g, strConst);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef idx[2] = {LLVMConstInt(I32Ty(b), 0, 0), LLVMConstInt(I32Ty(b), 0, 0)};
    return LLVMConstGEP2(strType, g, idx, 2);
}

// Emits the body of strata_strdup, which allocates through strata_alloc.
static void EmitStrataStrdupBody(Builder* b)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->builder);

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMTypeRef i64Ty = I64Ty(b);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "entry");
    LLVMBasicBlockRef lenCond = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "len.cond");
    LLVMBasicBlockRef lenBody = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "len.body");
    LLVMBasicBlockRef allocBB = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "alloc");
    LLVMBasicBlockRef copyCond = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "copy.cond");
    LLVMBasicBlockRef copyBody = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "copy.body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->ctx, b->strdupFn, "done");

    LLVMValueRef s = LLVMGetParam(b->strdupFn, 0);
    LLVMValueRef zero = LLVMConstInt(i64Ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(i64Ty, 1, 0);

    // Both loop counters are allocas in the ENTRY block - an alloca in a
    // non-entry block makes LLVM's x86 codegen treat the frame as dynamic and
    // emit a __chkstk stack probe.
    LLVMPositionBuilderAtEnd(b->builder, entry);
    LLVMValueRef iSlot = LLVMBuildAlloca(b->builder, i64Ty, "i");
    LLVMValueRef jSlot = LLVMBuildAlloca(b->builder, i64Ty, "j");
    LLVMBuildStore(b->builder, zero, iSlot);
    LLVMBuildBr(b->builder, lenCond);

    LLVMPositionBuilderAtEnd(b->builder, lenCond);
    LLVMValueRef i = LLVMBuildLoad2(b->builder, i64Ty, iSlot, "i");
    LLVMValueRef si[1] = {i};
    LLVMValueRef c = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, s, si, 1, "si"), "c");
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, c, LLVMConstNull(i8Ty), "isn");
    LLVMBuildCondBr(b->builder, isNull, allocBB, lenBody);

    LLVMPositionBuilderAtEnd(b->builder, lenBody);
    LLVMBuildStore(b->builder, LLVMBuildAdd(b->builder, i, one, "i1"), iSlot);
    LLVMBuildBr(b->builder, lenCond);

    LLVMPositionBuilderAtEnd(b->builder, allocBB);
    LLVMValueRef len = LLVMBuildLoad2(b->builder, i64Ty, iSlot, "n");
    LLVMValueRef allocArgs[1] = {LLVMBuildAdd(b->builder, len, one, "size")};
    StrataAllocFn(b);
    LLVMValueRef d = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, allocArgs, 1, "d");
    LLVMBuildStore(b->builder, zero, jSlot);
    LLVMBuildBr(b->builder, copyCond);

    LLVMPositionBuilderAtEnd(b->builder, copyCond);
    LLVMValueRef j = LLVMBuildLoad2(b->builder, i64Ty, jSlot, "j");
    LLVMValueRef jLe = LLVMBuildICmp(b->builder, LLVMIntULE, j, len, "jle");
    LLVMBuildCondBr(b->builder, jLe, copyBody, done);

    LLVMPositionBuilderAtEnd(b->builder, copyBody);
    LLVMValueRef sj[1] = {j};
    LLVMValueRef dAddr = LLVMBuildGEP2(b->builder, i8Ty, d, sj, 1, "dj");
    LLVMValueRef byte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, s, sj, 1, "sj"), "b");
    LLVMBuildStore(b->builder, byte, dAddr);
    LLVMBuildStore(b->builder, LLVMBuildAdd(b->builder, j, one, "j1"), jSlot);
    LLVMBuildBr(b->builder, copyCond);

    LLVMPositionBuilderAtEnd(b->builder, done);
    LLVMBuildRet(b->builder, d);

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->builder, savedBlock);
    }
}

LLVMValueRef StrataStrdupFn(Builder* b)
{
    if (!b->strdupFn)
    {
        LLVMTypeRef params[1] = {b->ptrTy};
        b->strdupFnType = LLVMFunctionType(b->ptrTy, params, 1, 0);
        b->strdupFn = LLVMAddFunction(b->mod, "strata_strdup", b->strdupFnType);
        MakeHelperModuleLocal(b, b->strdupFn);
        EmitStrataStrdupBody(b);
    }
    return b->strdupFn;
}

// Emits the body of strata_str_eq(a, alen, b, blen) -> bool: CONTENT
// equality for string `==`/`!=`. Never compares the fat pointers directly:
// lengths mismatch first (fast out), then pointer identity (fast in —
// covers the same literal/alias, moves, and both-empty {null, 0}), then a
// byte-wise loop over the shared length. Self-contained: no alloc, no free,
// no host symbols — works identically in AOT and JIT. The ABI return type is
// widened to i32 like every user function returning a sub-32-bit scalar (see
// WidenRetType): the full register always holds 0/1, so the helper is
// callable from C hosts and from JIT'd code under the one convention.
static void EmitStrataStrEqBody(Builder* b)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->builder);

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMTypeRef i64Ty = I64Ty(b);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "entry");
    LLVMBasicBlockRef lenEq = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "len.eq");
    LLVMBasicBlockRef loopCond = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "cmp.cond");
    LLVMBasicBlockRef loopBody = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "cmp.body");
    LLVMBasicBlockRef falseBB = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "neq");
    LLVMBasicBlockRef trueBB = LLVMAppendBasicBlockInContext(b->ctx, b->strEqFn, "eq");

    LLVMValueRef a = LLVMGetParam(b->strEqFn, 0);
    LLVMValueRef alen = LLVMGetParam(b->strEqFn, 1);
    LLVMValueRef bb = LLVMGetParam(b->strEqFn, 2);
    LLVMValueRef blen = LLVMGetParam(b->strEqFn, 3);

    LLVMValueRef zero = LLVMConstInt(i64Ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(i64Ty, 1, 0);

    // Length fast-out: unequal lengths can never be equal.
    LLVMPositionBuilderAtEnd(b->builder, entry);
    LLVMValueRef lenMatch = LLVMBuildICmp(b->builder, LLVMIntEQ, alen, blen, "lenm");
    LLVMBuildCondBr(b->builder, lenMatch, lenEq, falseBB);

    // Pointer identity fast-in: the same buffer is trivially equal.
    LLVMPositionBuilderAtEnd(b->builder, lenEq);
    LLVMValueRef sameBuf = LLVMBuildICmp(b->builder, LLVMIntEQ, a, bb, "sambuf");
    LLVMBuildCondBr(b->builder, sameBuf, trueBB, loopCond);

    // Byte-wise compare over [0, alen).
    LLVMPositionBuilderAtEnd(b->builder, loopCond);
    LLVMValueRef iPhi = LLVMBuildPhi(b->builder, i64Ty, "i");
    LLVMValueRef iNext = LLVMBuildAdd(b->builder, iPhi, one, "i1");
    LLVMValueRef inVals[2] = {zero, iNext};
    LLVMBasicBlockRef inBlocks[2] = {lenEq, loopBody};
    LLVMAddIncoming(iPhi, inVals, inBlocks, 2);
    LLVMValueRef iLt = LLVMBuildICmp(b->builder, LLVMIntULT, iPhi, alen, "ilt");
    LLVMBuildCondBr(b->builder, iLt, loopBody, trueBB);

    LLVMPositionBuilderAtEnd(b->builder, loopBody);
    LLVMValueRef idx[1] = {iPhi};
    LLVMValueRef aByte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, a, idx, 1, "ai"), "ab");
    LLVMValueRef bByte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, bb, idx, 1, "bi"), "bb");
    LLVMValueRef bytesEq = LLVMBuildICmp(b->builder, LLVMIntEQ, aByte, bByte, "beq");
    LLVMBuildCondBr(b->builder, bytesEq, loopCond, falseBB);

    LLVMPositionBuilderAtEnd(b->builder, trueBB);
    LLVMBuildRet(b->builder, LLVMConstInt(I32Ty(b), 1, 0));

    LLVMPositionBuilderAtEnd(b->builder, falseBB);
    LLVMBuildRet(b->builder, LLVMConstInt(I32Ty(b), 0, 0));

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->builder, savedBlock);
    }
}

LLVMValueRef StrataStrEqFn(Builder* b)
{
    if (!b->strEqFn)
    {
        // Widened i32 ABI return (WidenRetWidth convention), not i1: the
        // full register holds 0/1. Callers narrow back to i1 with an
        // `icmp ne` so branches/PHIs stay i1.
        LLVMTypeRef params[4] = {b->ptrTy, I64Ty(b), b->ptrTy, I64Ty(b)};
        b->strEqFnType = LLVMFunctionType(I32Ty(b), params, 4, 0);
        b->strEqFn = LLVMAddFunction(b->mod, "strata_str_eq", b->strEqFnType);
        MakeHelperModuleLocal(b, b->strEqFn);
        EmitStrataStrEqBody(b);
    }
    return b->strEqFn;
}

// Declares (once) the module-local helper `i64 @strata_cstrlen(ptr)`:
// NUL-terminated length, NULL -> 0. Out-of-line (unlike BuildStrLen's
// inlined loop) because equality helpers are emitted into their OWN
// function bodies — an inlined loop would allocate/branch in the wrong
// function. Self-contained like strata_str_eq: no host symbols, identical
// in AOT and JIT.
static void EmitStrataCStrLenBody(Builder* b)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->builder);

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMTypeRef i64Ty = I64Ty(b);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->ctx, b->csLenFn, "entry");
    LLVMBasicBlockRef loopCond = LLVMAppendBasicBlockInContext(b->ctx, b->csLenFn, "len.cond");
    LLVMBasicBlockRef loopBody = LLVMAppendBasicBlockInContext(b->ctx, b->csLenFn, "len.body");
    LLVMBasicBlockRef nullBB = LLVMAppendBasicBlockInContext(b->ctx, b->csLenFn, "len.null");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->ctx, b->csLenFn, "len.done");

    LLVMValueRef s = LLVMGetParam(b->csLenFn, 0);
    LLVMValueRef zero = LLVMConstInt(i64Ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(i64Ty, 1, 0);

    // A NULL pointer has length 0 (a zero-initialized `cstring` field).
    LLVMPositionBuilderAtEnd(b->builder, entry);
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, s, LLVMConstNull(b->ptrTy), "isnull");
    LLVMBuildCondBr(b->builder, isNull, nullBB, loopCond);

    LLVMPositionBuilderAtEnd(b->builder, nullBB);
    LLVMBuildBr(b->builder, done);

    LLVMPositionBuilderAtEnd(b->builder, loopCond);
    LLVMValueRef iPhi = LLVMBuildPhi(b->builder, i64Ty, "i");
    LLVMValueRef iNext = LLVMBuildAdd(b->builder, iPhi, one, "i1");
    LLVMValueRef inVals[2] = {zero, iNext};
    LLVMBasicBlockRef inBlocks[2] = {entry, loopBody};
    LLVMAddIncoming(iPhi, inVals, inBlocks, 2);
    LLVMValueRef idx[1] = {iPhi};
    LLVMValueRef byte = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, s, idx, 1, "p"), "c");
    LLVMValueRef isNul = LLVMBuildICmp(b->builder, LLVMIntEQ, byte, LLVMConstNull(i8Ty), "nul");
    LLVMBuildCondBr(b->builder, isNul, done, loopBody);

    LLVMPositionBuilderAtEnd(b->builder, loopBody);
    LLVMBuildBr(b->builder, loopCond);

    LLVMPositionBuilderAtEnd(b->builder, done);
    LLVMValueRef lenPhi = LLVMBuildPhi(b->builder, i64Ty, "len");
    LLVMValueRef lenVals[2] = {zero, iPhi};
    LLVMBasicBlockRef lenBlocks[2] = {nullBB, loopCond};
    LLVMAddIncoming(lenPhi, lenVals, lenBlocks, 2);
    LLVMBuildRet(b->builder, lenPhi);

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->builder, savedBlock);
    }
}

LLVMValueRef StrataCStrLenFn(Builder* b)
{
    if (!b->csLenFn)
    {
        LLVMTypeRef params[1] = {b->ptrTy};
        b->csLenFnType = LLVMFunctionType(I64Ty(b), params, 1, 0);
        b->csLenFn = LLVMAddFunction(b->mod, "strata_cstrlen", b->csLenFnType);
        MakeHelperModuleLocal(b, b->csLenFn);
        EmitStrataCStrLenBody(b);
    }

    return b->csLenFn;
}

// Emits substring(s, start, len) — a NEW owned string copying s[start ..
// start+len). The source is borrowed, so literals, locals, and array
// elements all work, and the source stays live. Out-of-range slices panic
// under AOT; under the JIT they report through strata_oob and yield an
// empty string, mirroring element indexing. Indices cross as i64, so
// negative values fail the unsigned range check below.
Value EmitSubstringBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    Value v = EmitExpr(b, arg0);

    // The source is only read: a fresh `substring(make_str(), ...)` source is
    // a temporary (the slice is a separate copy).
    BorrowTemp(b, v, TypeDescMake(I32Ty(b), 0, NULL));

    if (!TypeDescIsString(&v.typeDesc))
    {
        if (b->diag)
        {
            DiagErrorFmt(b->diag, n->base.range, "substring on a non-string value");
        }

        return ZeroInt(b);
    }

    LLVMValueRef start = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));
    LLVMValueRef len = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 2)));
    LLVMValueRef slen = WidenLen(b, LLVMBuildExtractValue(b->builder, v.value, 1, "sub.len"));
    LLVMValueRef dataPtr = LLVMBuildExtractValue(b->builder, v.value, 0, "sub.ptr");
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);

    if (!b->boundsCheck)
    {
        LLVMValueRef directPtr = LLVMBuildGEP2(b->builder, i8Ty, dataPtr, &start, 1, "sub.src");

        return ValueMake(BuildOwnedStringFat(b, directPtr, len), StringFatDesc(b));
    }

    // Unsigned range check: start > slen, or len > slen - start (the
    // subtraction wraps when start > slen, but badStart is already set).
    LLVMValueRef badStart = LLVMBuildICmp(b->builder, LLVMIntUGT, start, slen, "sub.oob1");
    LLVMValueRef remain = LLVMBuildSub(b->builder, slen, start, "sub.rem");
    LLVMValueRef badLen = LLVMBuildICmp(b->builder, LLVMIntUGT, len, remain, "sub.oob2");
    LLVMValueRef oob = LLVMBuildOr(b->builder, badStart, badLen, "sub.oob");

    if (!b->jitMode)
    {
        LLVMBasicBlockRef oobBB = NewBb(b, "sub.oob");
        LLVMBasicBlockRef okBB = NewBb(b, "sub.ok");
        LLVMBuildCondBr(b->builder, oob, oobBB, okBB);
        b->terminated = true;

        PositionAtEnd(b, oobBB);
        EmitPanic(b, "substring out of bounds");
        b->terminated = true;

        PositionAtEnd(b, okBB);
        LLVMValueRef srcPtr = LLVMBuildGEP2(b->builder, i8Ty, dataPtr, &start, 1, "sub.src");

        return ValueMake(BuildOwnedStringFat(b, srcPtr, len), StringFatDesc(b));
    }

    LLVMBasicBlockRef oobBB = NewBb(b, "sub.oob");
    LLVMBasicBlockRef okBB = NewBb(b, "sub.ok");
    LLVMBasicBlockRef mergeBB = NewBb(b, "sub.merge");
    LLVMBuildCondBr(b->builder, oob, oobBB, okBB);
    b->terminated = true;

    PositionAtEnd(b, oobBB);

    // A bookkeeping re-resolution of the same expression (null-the-source
    // after a move) is not a new access: the access itself already reported.
    if (!b->nullStoreLValue)
    {
        EmitOobReport(b);
    }

    LLVMValueRef emptyFat = BuildOwnedStringFat(b, EmptyStrConstant(b), LLVMConstInt(I64Ty(b), 0, 0));
    Br(b, mergeBB);
    LLVMBasicBlockRef oobEnd = LLVMGetInsertBlock(b->builder);

    PositionAtEnd(b, okBB);
    LLVMValueRef srcPtr = LLVMBuildGEP2(b->builder, i8Ty, dataPtr, &start, 1, "sub.src");
    LLVMValueRef fat = BuildOwnedStringFat(b, srcPtr, len);
    Br(b, mergeBB);
    LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(b->builder);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->builder, ArrayStructType(b), "sub");
    LLVMValueRef incomingVals[2] = {emptyFat, fat};
    LLVMBasicBlockRef incomingBlocks[2] = {oobEnd, okEnd};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

    return ValueMake(phi, StringFatDesc(b));
}

// The length of the text in a `byte[N]` buffer: up to its first NUL, or all N bytes.
static LLVMValueRef BuildBoundedStrLen(Builder* b, LLVMValueRef data, ArrayLength capacity)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef limit = LLVMConstInt(I64Ty(b), (unsigned long long)capacity, 0);
    LLVMBasicBlockRef cond = NewBb(b, "bsl.cond");
    LLVMBasicBlockRef body = NewBb(b, "bsl.body");
    LLVMBasicBlockRef next = NewBb(b, "bsl.next");
    LLVMBasicBlockRef done = NewBb(b, "bsl.done");
    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "bsl.i");
    LLVMBuildStore(b->builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "i");
    LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntULT, i, limit, "bsl.in"), body, done);
    b->terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef at[1] = {i};
    LLVMValueRef c = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, data, at, 1, "bsl.p"), "bsl.c");
    LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, c, LLVMConstInt(i8Ty, 0, 0), "bsl.nul"), done,
                    next);
    b->terminated = true;

    PositionAtEnd(b, next);
    LLVMBuildStore(b->builder, LLVMBuildAdd(b->builder, i, LLVMConstInt(I64Ty(b), 1, 0), "bsl.n"), iSlot);
    Br(b, cond);

    PositionAtEnd(b, done);
    return LLVMBuildLoad2(b->builder, I64Ty(b), iSlot, "bsl.len");
}

// `(cstring)buf`, `(string)buf` and `(cstring)s` (see CastTextKind).
Value EmitTextCast(Builder* b, CastExpr* cast)
{
    TypeDesc target = Resolve(b, &cast->type);

    if (cast->textKind == CastStringToCString)
    {
        Value s = EmitExpr(b, cast->operand);
        // A temporary string lives to the end of the statement, like a comparison operand.
        BorrowTemp(b, s, target);
        return ValueMake(BuildExternStringPtr(b, s), target);
    }

    LValue lv = EmitLValue(b, cast->operand);
    LLVMValueRef data;
    ArrayLength capacity;

    if (lv.valid && !lv.vectorLane)
    {
        data = lv.ptr;
        capacity = lv.typeDesc.fixedLength;
    }
    else
    {
        Value v = EmitExpr(b, cast->operand);
        data = EntryAlloca(b, v.typeDesc.type, "textbuf");
        LLVMBuildStore(b->builder, v.value, data);
        capacity = v.typeDesc.fixedLength;
    }

    if (cast->textKind == CastBufferToCString || cast->textKind == CastFixedToPtr)
    {
        return ValueMake(data, target);
    }

    return ValueMake(BuildOwnedStringFat(b, data, BuildBoundedStrLen(b, data, capacity)), target);
}

Value EmitStrLiteral(Builder* b, StrLiteral* literal)
{
    size_t len = literal->length;
    // The constant carries its NUL terminator (flag 0 in this LLVM
    // build), upholding the fat-string invariant: NUL at [len].
    LLVMValueRef strConst = LLVMConstStringInContext(b->ctx, literal->value, (unsigned)len, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);

    char* gName = ArenaFormat(b->arena, ".str.%d", b->strLitCount++);
    LLVMValueRef global = LLVMAddGlobal(b->mod, strType, gName);
    LLVMSetInitializer(global, strConst);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(global, 1);
    LLVMSetGlobalConstant(global, 1);

    LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
    LLVMValueRef idx[2] = {zero, zero};
    LLVMValueRef gep = LLVMConstGEP2(strType, global, idx, 2);

    // A literal is a borrowed fat {ptr, len, cap}: no allocation, NUL at
    // [len]. cap is len+1 (the constant carries the terminator byte).
    // Owning consumers heap-copy via EmitOwnedValue.
    TypeDesc td = StringFatDesc(b);
    LLVMValueRef fat = LLVMConstNamedStruct(ArrayStructType(b),
                                            (LLVMValueRef[3]){gep, LLVMConstInt(I32Ty(b), (unsigned long long)len, 0),
                                                              LLVMConstInt(I32Ty(b), (unsigned long long)len + 1, 0)},
                                            3);

    return ValueMake(fat, td);
}
