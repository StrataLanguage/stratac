// Builder: Equality: ==/!= helpers for bytes, boxes, handles and structs.
#include "Codegen/Builder/BuilderInternal.h"

// Equality checking.
//
// `==` on aggregates is STRUCTURAL. Per compared type, one of two paths:
//
// - Byte path (memcmp-like): scalars/handles, fixed arrays and structs
// whose fields are all byte-comparable. Valid because every Strata-created
// struct value is zero-initialized (padding included), so equal values
// have equal bytes.
// - Structural path: floats/doubles use `fcmp oeq` (IEEE-754: -0.0 == 0.0,
// NaN != NaN — bytes would disagree on both), strings per-element content
// (strata_str_eq), cstrings by content (strata_cstrlen + strata_str_eq),
// dynamic arrays element-wise, structs member-wise, and
// boxes AUTO-DEREF into a structural compare of the cell (null-aware for
// `T?`; identity only for opaque inners, where the cell IS the value).
//
// The structural path is emitted into module-local per-type helpers
// (`strata_eq_<type>`: (ptr, ptr) -> i1, cached in eqHelpers), generated
// on demand and self-contained like strata_str_eq — no host symbols, so AOT
// and JIT behave identically.
static bool TypeDescIsByteEq(Builder* b, const TypeDesc* td)
{
    if (TypeDescIsString(td) || TypeDescIsCString(td) || TypeDescHasArrayShape(td) || TypeDescIsFloat(td))
    {
        return false;
    }

    // Boxes auto-deref in equality (structural compare of the cell), so
    // their pointer bits never decide.
    if (TypeDescIsBox(td))
    {
        return false;
    }

    if (td->structTypeName)
    {
        const StructType* st = TypeRegistryFind(&b->registry, td->structTypeName);

        if (!st || st->isExtern)
        {
            return false;
        }

        for (size_t i = 0; i < st->fields.count; i++)
        {
            FieldDecl* f = (FieldDecl*)VecGet(&st->fields, i);
            TypeDesc fieldTd = Resolve(b, &f->type);

            if (!TypeDescIsByteEq(b, &fieldTd))
            {
                return false;
            }
        }

        return true;
    }

    if (TypeDescIsFixedArray(td))
    {
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        return TypeDescIsByteEq(b, &elemTd);
    }

    return true;
}

static const char* EqHelperKey(Builder* b, const TypeDesc* td)
{
    if (td->structTypeName)
    {
        return td->structTypeName;
    }

    if (TypeDescIsString(td))
    {
        return "string";
    }

    if (TypeDescIsFixedArray(td))
    {
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        return ArenaFormat(b->arena, "%s[%ld]", EqHelperKey(b, &elemTd), td->fixedLength);
    }

    if (TypeDescHasArrayShape(td))
    {
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        return ArenaFormat(b->arena, "%s[]", EqHelperKey(b, &elemTd));
    }

    char* printed = LLVMPrintTypeToString(td->type);
    char* key = ArenaStrdup(b->arena, printed);
    LLVMDisposeMessage(printed);
    return key;
}

// Sanitizes a cache key into a valid LLVM identifier suffix. Collisions
// can't mis-link: helpers are always resolved through the eqHelpers cache,
// never by name.
static const char* EqHelperName(Builder* b, const char* key)
{
    size_t n = strlen(key);
    char* out = (char*)ArenaAlloc(b->arena, n + 1);

    for (size_t i = 0; i < n; i++)
    {
        char c = key[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[i] = ok ? c : '_';
    }

    out[n] = '\0';
    return ArenaFormat(b->arena, "strata_eq_%s", out);
}

// Compares `byteLen` bytes at aPtr/bPtr (memcmp semantics); branches to
// failBB on the first difference, to the returned block on equality.
static LLVMBasicBlockRef EmitEqBytes(Builder* b, LLVMValueRef fn, LLVMValueRef aPtr, LLVMValueRef bPtr,
                                     LLVMValueRef byteLen, LLVMBasicBlockRef failBB)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->ctx);
    LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);

    LLVMBasicBlockRef from = LLVMGetInsertBlock(b->builder);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.cmp.cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.cmp.body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.cmp.done");

    LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntUGT, byteLen, zero, "eq.has"), cond, done);

    LLVMPositionBuilderAtEnd(b->builder, cond);
    LLVMValueRef i = LLVMBuildPhi(b->builder, I64Ty(b), "eq.i");
    LLVMValueRef iNext = LLVMBuildAdd(b->builder, i, one, "eq.i1");
    LLVMValueRef inRange = LLVMBuildICmp(b->builder, LLVMIntULT, i, byteLen, "eq.ilt");
    LLVMBuildCondBr(b->builder, inRange, body, done);

    LLVMPositionBuilderAtEnd(b->builder, body);
    LLVMValueRef idx[1] = {i};
    LLVMValueRef aByte
        = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, aPtr, idx, 1, "eq.a"), "eq.ab");
    LLVMValueRef bByte
        = LLVMBuildLoad2(b->builder, i8Ty, LLVMBuildGEP2(b->builder, i8Ty, bPtr, idx, 1, "eq.b"), "eq.bb");
    LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, aByte, bByte, "eq.beq"), cond, failBB);

    LLVMValueRef inVals[2] = {zero, iNext};
    LLVMBasicBlockRef inBlocks[2] = {from, body};
    LLVMAddIncoming(i, inVals, inBlocks, 2);

    LLVMPositionBuilderAtEnd(b->builder, done);
    return done;
}

// Compares the HANDLE VALUES held in two ^Handle cells: loads both and
// icmps (a handle is a pointer-sized value, the cell is its storage).
static LLVMValueRef BuildHandleCellEq(Builder* b, LLVMValueRef aCell, LLVMValueRef bCell)
{
    LLVMValueRef a = LLVMBuildLoad2(b->builder, b->ptrTy, aCell, "eq.ha");
    LLVMValueRef bv = LLVMBuildLoad2(b->builder, b->ptrTy, bCell, "eq.hb");

    return LLVMBuildICmp(b->builder, LLVMIntEQ, a, bv, "eq.hdl");
}

// Deep-compares two box cells (aCell/bCell — heap pointers; NULL when empty
// for optionals) of inner type `innerTd`, AUTO-DEREFING like every other
// box use. Incomplete-struct inners hold the T* ITSELF (DerefBoxValue's
// identity unwrap), so their "deep" compare is the cell values; handle
// inners hold the handle VALUE, so both cells are loaded and the handles
// compared. `isOptional` adds null-awareness: empty == empty, empty !=
// non-empty. Branches to failBB on inequality; positions the builder at the
// end of (and returns) the equal-continuation block.
LLVMBasicBlockRef EmitBoxCellEq(Builder* b, LLVMValueRef fn, LLVMValueRef aCell, LLVMValueRef bCell,
                                const TypeDesc* innerTd, bool isOptional, LLVMBasicBlockRef failBB)
{
    bool opaqueInner = innerTd->structTypeName && TypeRegistryIsOpaque(&b->registry, innerTd->structTypeName);
    bool handleInner = opaqueInner && IsHandleType(&b->registry, innerTd->structTypeName);

    LLVMTypeRef eqParams[2] = {b->ptrTy, b->ptrTy};
    LLVMTypeRef eqFnTy = LLVMFunctionType(I1Ty(b), eqParams, 2, 0);
    LLVMValueRef eqArgs[2] = {aCell, bCell};
    LLVMValueRef eq;

    if (isOptional)
    {
        LLVMValueRef nullP = LLVMConstNull(b->ptrTy);
        LLVMValueRef nullA = LLVMBuildICmp(b->builder, LLVMIntEQ, aCell, nullP, "eq.na");
        LLVMValueRef nullB = LLVMBuildICmp(b->builder, LLVMIntEQ, bCell, nullP, "eq.nb");

        LLVMBasicBlockRef maybeDeep = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.nchk");
        LLVMBasicBlockRef deep = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.deep");
        LLVMBasicBlockRef eqNext = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.next");

        // One empty and one not: unequal. Both empty: equal. Both non-empty:
        // deref and compare the cells' values.
        LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, nullA, nullB, "eq.same"), maybeDeep, failBB);

        LLVMPositionBuilderAtEnd(b->builder, maybeDeep);
        LLVMBuildCondBr(b->builder, nullA, eqNext, deep);

        LLVMPositionBuilderAtEnd(b->builder, deep);
        eq = handleInner ? BuildHandleCellEq(b, aCell, bCell)
             : opaqueInner
                 ? LLVMBuildICmp(b->builder, LLVMIntEQ, aCell, bCell, "eq.box")
                 : LLVMBuildCall2(b->builder, eqFnTy, EnsureEqHelper(b, innerTd, false), eqArgs, 2, "eq.cell");
        LLVMBuildCondBr(b->builder, eq, eqNext, failBB);

        LLVMPositionBuilderAtEnd(b->builder, eqNext);
        return eqNext;
    }

    eq = handleInner   ? BuildHandleCellEq(b, aCell, bCell)
         : opaqueInner ? LLVMBuildICmp(b->builder, LLVMIntEQ, aCell, bCell, "eq.box")
                       : LLVMBuildCall2(b->builder, eqFnTy, EnsureEqHelper(b, innerTd, false), eqArgs, 2, "eq.cell");
    LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.next");
    LLVMBuildCondBr(b->builder, eq, next, failBB);
    LLVMPositionBuilderAtEnd(b->builder, next);
    return next;
}

// Compares two values AT aPtr/bPtr of type `td`: branches to failBB on
// inequality and returns (with the builder positioned at its end) the block
// reached when equal. Strings use the strata_str_eq content compare,
// aggregates delegate to their (cached) per-type helper, boxes deref (see
// EmitBoxCellEq). `externBoxCtx` marks walks inside host-layout extern
// structs, where ^T members are host-opaque pointers that compare by
// identity.
static LLVMBasicBlockRef EmitEqLeaf(Builder* b, LLVMValueRef fn, LLVMValueRef aPtr, LLVMValueRef bPtr,
                                    const TypeDesc* td, LLVMBasicBlockRef failBB, bool externBoxCtx)
{
    LLVMValueRef eq;

    if (TypeDescIsString(td))
    {
        LLVMValueRef aData = LLVMBuildLoad2(b->builder, b->ptrTy, ArrayDataPtr(b, aPtr), "eq.sa");
        LLVMValueRef aLen = WidenLen(b, LLVMBuildLoad2(b->builder, I32Ty(b), ArrayLenPtr(b, aPtr), "eq.sla"));
        LLVMValueRef bData = LLVMBuildLoad2(b->builder, b->ptrTy, ArrayDataPtr(b, bPtr), "eq.sb");
        LLVMValueRef bLen = WidenLen(b, LLVMBuildLoad2(b->builder, I32Ty(b), ArrayLenPtr(b, bPtr), "eq.slb"));

        LLVMValueRef args[4] = {aData, aLen, bData, bLen};
        StrataStrEqFn(b);
        LLVMValueRef wide = LLVMBuildCall2(b->builder, b->strEqFnType, b->strEqFn, args, 4, "eq.str");
        eq = LLVMBuildICmp(b->builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "eq.str.b");
    }
    else if (TypeDescIsCString(td))
    {
        // `cstring` field/element: CONTENT equality (strata_cstrlen +
        // strata_str_eq) — never a raw pointer compare (two identical
        // literals may or may not share storage, so a pointer compare
        // would be nondeterministic).
        LLVMValueRef aData = LLVMBuildLoad2(b->builder, b->ptrTy, aPtr, "eq.csa");
        LLVMValueRef bData = LLVMBuildLoad2(b->builder, b->ptrTy, bPtr, "eq.csb");
        LLVMValueRef aLen = LLVMBuildCall2(b->builder, b->csLenFnType, StrataCStrLenFn(b), &aData, 1, "eq.csla");
        LLVMValueRef bLen = LLVMBuildCall2(b->builder, b->csLenFnType, StrataCStrLenFn(b), &bData, 1, "eq.cslb");

        LLVMValueRef args[4] = {aData, aLen, bData, bLen};
        StrataStrEqFn(b);
        LLVMValueRef wide = LLVMBuildCall2(b->builder, b->strEqFnType, b->strEqFn, args, 4, "eq.cs");
        eq = LLVMBuildICmp(b->builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "eq.cs.b");
    }
    else if (td->structTypeName || TypeDescHasArrayShape(td) || TypeDescIsFixedArray(td))
    {
        LLVMValueRef helper = EnsureEqHelper(b, td, externBoxCtx);
        LLVMTypeRef params[2] = {b->ptrTy, b->ptrTy};
        LLVMTypeRef fnTy = LLVMFunctionType(I1Ty(b), params, 2, 0);
        LLVMValueRef args[2] = {aPtr, bPtr};
        eq = LLVMBuildCall2(b->builder, fnTy, helper, args, 2, "eq.agg");
    }
    else if (TypeDescIsBox(td))
    {
        LLVMValueRef aCell = LLVMBuildLoad2(b->builder, b->ptrTy, aPtr, "eq.ca");
        LLVMValueRef bCell = LLVMBuildLoad2(b->builder, b->ptrTy, bPtr, "eq.cb");

        if (externBoxCtx || !td->boxInner)
        {
            // Host-opaque ^T member, or the degenerate ^string whose cell
            // IS a char*: the cell itself decides.
            eq = LLVMBuildICmp(b->builder, LLVMIntEQ, aCell, bCell, "eq.box");
        }
        else
        {
            TypeDesc innerTd = Resolve(b, td->boxInner);
            return EmitBoxCellEq(b, fn, aCell, bCell, &innerTd, TypeDescIsOptional(td), failBB);
        }
    }
    else
    {
        // Scalars, handles, enums: load both, compare the values (floats
        // with fcmp oeq — IEEE-754, matching C).
        LLVMValueRef av = LLVMBuildLoad2(b->builder, td->type, aPtr, "eq.av");
        LLVMValueRef bv = LLVMBuildLoad2(b->builder, td->type, bPtr, "eq.bv");
        eq = TypeDescIsFloat(td) ? FcmpByName(b->builder, "oeq", av, bv)
                                 : LLVMBuildICmp(b->builder, LLVMIntEQ, av, bv, "eq");
    }

    LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.next");
    LLVMBuildCondBr(b->builder, eq, next, failBB);
    LLVMPositionBuilderAtEnd(b->builder, next);
    return next;
}

// Emits the body of the (ptr, ptr) -> i1 equality helper for `td`.
// `externBoxCtx` is true when this walk started inside a host-layout extern
// struct: ^T members there are host-opaque pointers and compare by identity.
static void EmitEqHelperBody(Builder* b, LLVMValueRef fn, const TypeDesc* td, bool externBoxCtx)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->builder);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->ctx, fn, "entry");
    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(b->ctx, fn, "fail");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->ctx, fn, "done");

    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef bp = LLVMGetParam(fn, 1);
    LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);

    LLVMPositionBuilderAtEnd(b->builder, entry);

    if (td->structTypeName)
    {
        // Struct: member-wise. Each field's compare chains into the next;
        // the first mismatch branches to fail.
        const StructType* st = TypeRegistryFind(&b->registry, td->structTypeName);
        bool fieldBoxCtx = externBoxCtx || (st && st->isExtern);

        for (size_t i = 0; st && i < st->fields.count; i++)
        {
            FieldDecl* f = (FieldDecl*)VecGet(&st->fields, i);
            TypeDesc fieldTd = Resolve(b, &f->type);

            LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)i))};
            LLVMValueRef fa = LLVMBuildGEP2(b->builder, td->type, a, idxs, 2, "eq.fa");
            LLVMValueRef fb = LLVMBuildGEP2(b->builder, td->type, bp, idxs, 2, "eq.fb");
            EmitEqLeaf(b, fn, fa, fb, &fieldTd, fail, fieldBoxCtx);
        }

        LLVMBuildBr(b->builder, done);
    }
    else if (TypeDescIsFixedArray(td))
    {
        // Fixed array: unrolled element-wise compare.
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

        for (ArrayLength k = 0; k < td->fixedLength; k++)
        {
            LLVMValueRef idx[1] = {IdxConst(b, (unsigned)k)};
            LLVMValueRef ka = LLVMBuildGEP2(b->builder, elemTy, a, idx, 1, "eq.ka");
            LLVMValueRef kb = LLVMBuildGEP2(b->builder, elemTy, bp, idx, 1, "eq.kb");
            EmitEqLeaf(b, fn, ka, kb, &elemTd, fail, externBoxCtx);
        }

        LLVMBuildBr(b->builder, done);
    }
    else if (TypeDescHasArrayShape(td))
    {
        // Dynamic array: lengths must match (both-empty is equal), then a
        // byte compare for byte-comparable elements or an element-wise
        // loop otherwise (never a raw buffer/fat compare).
        LLVMValueRef aLen = WidenLen(b, LLVMBuildLoad2(b->builder, I32Ty(b), ArrayLenPtr(b, a), "eq.ala"));
        LLVMValueRef bLen = WidenLen(b, LLVMBuildLoad2(b->builder, I32Ty(b), ArrayLenPtr(b, bp), "eq.alb"));
        LLVMValueRef aData = LLVMBuildLoad2(b->builder, b->ptrTy, ArrayDataPtr(b, a), "eq.ada");
        LLVMValueRef bData = LLVMBuildLoad2(b->builder, b->ptrTy, ArrayDataPtr(b, bp), "eq.adb");

        LLVMBasicBlockRef lenOk = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.len");
        LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, aLen, bLen, "eq.leneq"), lenOk, fail);

        LLVMPositionBuilderAtEnd(b->builder, lenOk);
        LLVMBasicBlockRef notEmpty = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.nempty");
        LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntEQ, aLen, zero, "eq.zero"), done, notEmpty);

        LLVMPositionBuilderAtEnd(b->builder, notEmpty);
        TypeDesc elemTd = Resolve(b, td->arrayInner);

        if (TypeDescIsByteEq(b, &elemTd))
        {
            LLVMBasicBlockRef bytesDone = EmitEqBytes(
                b, fn, aData, bData, LLVMBuildMul(b->builder, SizeOfConst(b, elemTd.type), aLen, "eq.bytes"), fail);
            LLVMPositionBuilderAtEnd(b->builder, bytesDone);
            LLVMBuildBr(b->builder, done);
        }
        else
        {
            LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

            LLVMBasicBlockRef from = LLVMGetInsertBlock(b->builder);
            LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.elem.cond");
            LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(b->ctx, fn, "eq.elem.body");

            LLVMBuildBr(b->builder, cond);

            LLVMPositionBuilderAtEnd(b->builder, cond);
            LLVMValueRef i = LLVMBuildPhi(b->builder, I64Ty(b), "eq.i");
            LLVMValueRef iNext = LLVMBuildAdd(b->builder, i, LLVMConstInt(I64Ty(b), 1, 0), "eq.i1");
            LLVMBuildCondBr(b->builder, LLVMBuildICmp(b->builder, LLVMIntULT, i, aLen, "eq.elt"), body, done);

            LLVMPositionBuilderAtEnd(b->builder, body);
            LLVMValueRef idx[1] = {i};
            LLVMValueRef ea = LLVMBuildGEP2(b->builder, elemTy, aData, idx, 1, "eq.ea");
            LLVMValueRef eb = LLVMBuildGEP2(b->builder, elemTy, bData, idx, 1, "eq.eb");
            LLVMBasicBlockRef eqEnd = EmitEqLeaf(b, fn, ea, eb, &elemTd, fail, externBoxCtx);
            LLVMPositionBuilderAtEnd(b->builder, eqEnd);
            LLVMBuildBr(b->builder, cond);

            LLVMValueRef inVals[2] = {zero, iNext};
            LLVMBasicBlockRef inBlocks[2] = {from, eqEnd};
            LLVMAddIncoming(i, inVals, inBlocks, 2);
        }
    }
    else
    {
        // Non-aggregate reached directly (defensive): single leaf compare.
        LLVMBasicBlockRef eqEnd = EmitEqLeaf(b, fn, a, bp, td, fail, externBoxCtx);
        LLVMPositionBuilderAtEnd(b->builder, eqEnd);
        LLVMBuildBr(b->builder, done);
    }

    LLVMPositionBuilderAtEnd(b->builder, done);
    LLVMBuildRet(b->builder, LLVMConstInt(I1Ty(b), 1, 0));

    LLVMPositionBuilderAtEnd(b->builder, fail);
    LLVMBuildRet(b->builder, LLVMConstInt(I1Ty(b), 0, 0));

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->builder, savedBlock);
    }
}

// Per-type equality helper: (ptr a, ptr b) -> i1, comparing the values the pointers address. Cached in eqHelpers;
// creating it before emitting the body keeps recursive type graphs (struct in struct, nested arrays) safe.
LLVMValueRef EnsureEqHelper(Builder* b, const TypeDesc* td, bool externBoxCtx)
{
    const char* baseKey = EqHelperKey(b, td);
    const char* key = externBoxCtx ? ArenaFormat(b->arena, "%s!x", baseKey) : baseKey;

    LLVMValueRef fn = (LLVMValueRef)StrMapGet(&b->eqHelpers, key);

    if (fn)
    {
        return fn;
    }

    LLVMTypeRef params[2] = {b->ptrTy, b->ptrTy};
    LLVMTypeRef fnTy = LLVMFunctionType(I1Ty(b), params, 2, 0);
    fn = LLVMAddFunction(b->mod, EqHelperName(b, key), fnTy);
    StrMapPut(&b->eqHelpers, (char*)key, fn);

    EmitEqHelperBody(b, fn, td, externBoxCtx);

    return fn;
}

// Parks an already-evaluated aggregate operand in a fresh entry alloca so
// the structural-equality helper can address it byte/field-wise.
LLVMValueRef SpillEqOperand(Builder* b, Value v)
{
    LLVMValueRef slot = EntryAlloca(b, v.typeDesc.type, "eqtmp");
    LLVMBuildStore(b->builder, v.value, slot);
    return slot;
}
