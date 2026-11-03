/* Builder: Equality: ==/!= helpers for bytes, boxes, handles and structs. */
#include "Codegen/Builder/BuilderInternal.h"

/* Equality checking.

   `==` on aggregates is STRUCTURAL. Per compared type, one of two paths:

   - Byte path (memcmp-like): scalars/handles, fixed arrays and structs
     whose fields are all byte-comparable. Valid because every Strata-created
     struct value is zero-initialized (padding included), so equal values
     have equal bytes.
   - Structural path: floats/doubles use `fcmp oeq` (IEEE-754: -0.0 == 0.0,
      NaN != NaN — bytes would disagree on both), strings per-element content
      (strata_str_eq), cstrings by content (strata_cstrlen + strata_str_eq),
      dynamic arrays element-wise, structs member-wise, and
      boxes AUTO-DEREF into a structural compare of the cell (null-aware for
      `T?`; identity only for opaque inners, where the cell IS the value).

   The structural path is emitted into module-local per-type helpers
   (`strata_eq_<type>`: (ptr, ptr) -> i1, cached in m_eqHelpers), generated
   on demand and self-contained like strata_str_eq — no host symbols, so AOT
   and JIT behave identically. */
static bool TypeDescIsByteEq(Builder* b, const TypeDesc* td)
{
    if (td->isString || td->isCString || TypeDescHasArrayShape(td) || td->isFloat)
    {
        return false;
    }

    /* Boxes auto-deref in equality (structural compare of the cell), so
       their pointer bits never decide. */
    if (TypeDescIsBox(td))
    {
        return false;
    }

    if (td->structTypeName)
    {
        const StructType* st = TypeRegistryFind(&b->m_registry, td->structTypeName);

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

    if (td->isFixedArray)
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

    if (td->isString)
    {
        return "string";
    }

    if (td->isFixedArray)
    {
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        return arena_format(b->m_arena, "%s[%ld]", EqHelperKey(b, &elemTd), td->fixedLength);
    }

    if (TypeDescHasArrayShape(td))
    {
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        return arena_format(b->m_arena, "%s[]", EqHelperKey(b, &elemTd));
    }

    char* printed = LLVMPrintTypeToString(td->type);
    char* key = arena_strdup(b->m_arena, printed);
    LLVMDisposeMessage(printed);
    return key;
}

/* Sanitizes a cache key into a valid LLVM identifier suffix. Collisions
   can't mis-link: helpers are always resolved through the m_eqHelpers cache,
   never by name. */
static const char* EqHelperName(Builder* b, const char* key)
{
    size_t n = strlen(key);
    char* out = (char*)arena_alloc(b->m_arena, n + 1);

    for (size_t i = 0; i < n; i++)
    {
        char c = key[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[i] = ok ? c : '_';
    }

    out[n] = '\0';
    return arena_format(b->m_arena, "strata_eq_%s", out);
}

/* Compares `byteLen` bytes at aPtr/bPtr (memcmp semantics); branches to
   failBB on the first difference, to the returned block on equality. */
static LLVMBasicBlockRef EmitEqBytes(Builder* b, LLVMValueRef fn, LLVMValueRef aPtr, LLVMValueRef bPtr,
                                     LLVMValueRef byteLen, LLVMBasicBlockRef failBB)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
    LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);

    LLVMBasicBlockRef from = LLVMGetInsertBlock(b->m_builder);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.cmp.cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.cmp.body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.cmp.done");

    LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntUGT, byteLen, zero, "eq.has"), cond, done);

    LLVMPositionBuilderAtEnd(b->m_builder, cond);
    LLVMValueRef i = LLVMBuildPhi(b->m_builder, I64Ty(b), "eq.i");
    LLVMValueRef iNext = LLVMBuildAdd(b->m_builder, i, one, "eq.i1");
    LLVMValueRef inRange = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, byteLen, "eq.ilt");
    LLVMBuildCondBr(b->m_builder, inRange, body, done);

    LLVMPositionBuilderAtEnd(b->m_builder, body);
    LLVMValueRef idx[1] = {i};
    LLVMValueRef aByte
        = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, aPtr, idx, 1, "eq.a"), "eq.ab");
    LLVMValueRef bByte
        = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, bPtr, idx, 1, "eq.b"), "eq.bb");
    LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, aByte, bByte, "eq.beq"), cond, failBB);

    LLVMValueRef inVals[2] = {zero, iNext};
    LLVMBasicBlockRef inBlocks[2] = {from, body};
    LLVMAddIncoming(i, inVals, inBlocks, 2);

    LLVMPositionBuilderAtEnd(b->m_builder, done);
    return done;
}

/* Per-type equality helper: (ptr a, ptr b) -> i1, comparing the values the
   pointers address. Cached in m_eqHelpers; creating it before emitting the
   body keeps recursive type graphs (struct in struct, nested arrays) safe. */

/* Compares the HANDLE VALUES held in two ^Handle cells: loads both and
   icmps (a handle is a pointer-sized value, the cell is its storage). */
static LLVMValueRef BuildHandleCellEq(Builder* b, LLVMValueRef aCell, LLVMValueRef bCell)
{
    LLVMValueRef a = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, aCell, "eq.ha");
    LLVMValueRef bv = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, bCell, "eq.hb");

    return LLVMBuildICmp(b->m_builder, LLVMIntEQ, a, bv, "eq.hdl");
}

/* Deep-compares two box cells (aCell/bCell — heap pointers; NULL when empty
   for optionals) of inner type `innerTd`, AUTO-DEREFING like every other
   box use. Incomplete-struct inners hold the T* ITSELF (DerefBoxValue's
   identity unwrap), so their "deep" compare is the cell values; handle
   inners hold the handle VALUE, so both cells are loaded and the handles
   compared. `isOptional` adds null-awareness: empty == empty, empty !=
   non-empty. Branches to failBB on inequality; positions the builder at the
   end of (and returns) the equal-continuation block. */
LLVMBasicBlockRef EmitBoxCellEq(Builder* b, LLVMValueRef fn, LLVMValueRef aCell, LLVMValueRef bCell,
                                const TypeDesc* innerTd, bool isOptional, LLVMBasicBlockRef failBB)
{
    bool opaqueInner = innerTd->structTypeName && TypeRegistryIsOpaque(&b->m_registry, innerTd->structTypeName);
    bool handleInner = opaqueInner && IsHandleType(&b->m_registry, innerTd->structTypeName);

    LLVMTypeRef eqParams[2] = {b->m_ptrTy, b->m_ptrTy};
    LLVMTypeRef eqFnTy = LLVMFunctionType(I1Ty(b), eqParams, 2, 0);
    LLVMValueRef eqArgs[2] = {aCell, bCell};
    LLVMValueRef eq;

    if (isOptional)
    {
        LLVMValueRef nullP = LLVMConstNull(b->m_ptrTy);
        LLVMValueRef nullA = LLVMBuildICmp(b->m_builder, LLVMIntEQ, aCell, nullP, "eq.na");
        LLVMValueRef nullB = LLVMBuildICmp(b->m_builder, LLVMIntEQ, bCell, nullP, "eq.nb");

        LLVMBasicBlockRef maybeDeep = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.nchk");
        LLVMBasicBlockRef deep = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.deep");
        LLVMBasicBlockRef eqNext = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.next");

        /* One empty and one not: unequal. Both empty: equal. Both non-empty:
           deref and compare the cells' values. */
        LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, nullA, nullB, "eq.same"), maybeDeep,
                        failBB);

        LLVMPositionBuilderAtEnd(b->m_builder, maybeDeep);
        LLVMBuildCondBr(b->m_builder, nullA, eqNext, deep);

        LLVMPositionBuilderAtEnd(b->m_builder, deep);
        eq = handleInner ? BuildHandleCellEq(b, aCell, bCell)
             : opaqueInner
                 ? LLVMBuildICmp(b->m_builder, LLVMIntEQ, aCell, bCell, "eq.box")
                 : LLVMBuildCall2(b->m_builder, eqFnTy, EnsureEqHelper(b, innerTd, false), eqArgs, 2, "eq.cell");
        LLVMBuildCondBr(b->m_builder, eq, eqNext, failBB);

        LLVMPositionBuilderAtEnd(b->m_builder, eqNext);
        return eqNext;
    }

    eq = handleInner   ? BuildHandleCellEq(b, aCell, bCell)
         : opaqueInner ? LLVMBuildICmp(b->m_builder, LLVMIntEQ, aCell, bCell, "eq.box")
                       : LLVMBuildCall2(b->m_builder, eqFnTy, EnsureEqHelper(b, innerTd, false), eqArgs, 2, "eq.cell");
    LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.next");
    LLVMBuildCondBr(b->m_builder, eq, next, failBB);
    LLVMPositionBuilderAtEnd(b->m_builder, next);
    return next;
}

/* Compares two values AT aPtr/bPtr of type `td`: branches to failBB on
   inequality and returns (with the builder positioned at its end) the block
   reached when equal. Strings use the strata_str_eq content compare,
   aggregates delegate to their (cached) per-type helper, boxes deref (see
   EmitBoxCellEq). `externBoxCtx` marks walks inside host-layout extern
   structs, where ^T members are host-opaque pointers that compare by
   identity. */
static LLVMBasicBlockRef EmitEqLeaf(Builder* b, LLVMValueRef fn, LLVMValueRef aPtr, LLVMValueRef bPtr,
                                    const TypeDesc* td, LLVMBasicBlockRef failBB, bool externBoxCtx)
{
    LLVMValueRef eq;

    if (td->isString)
    {
        LLVMValueRef aData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, aPtr), "eq.sa");
        LLVMValueRef aLen = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, aPtr), "eq.sla"));
        LLVMValueRef bData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, bPtr), "eq.sb");
        LLVMValueRef bLen = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, bPtr), "eq.slb"));

        LLVMValueRef args[4] = {aData, aLen, bData, bLen};
        StrataStrEqFn(b);
        LLVMValueRef wide = LLVMBuildCall2(b->m_builder, b->m_strEqFnType, b->m_strEqFn, args, 4, "eq.str");
        eq = LLVMBuildICmp(b->m_builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "eq.str.b");
    }
    else if (td->isCString)
    {
        /* `cstring` field/element: CONTENT equality (strata_cstrlen +
           strata_str_eq) — never a raw pointer compare (two identical
           literals may or may not share storage, so a pointer compare
           would be nondeterministic). */
        LLVMValueRef aData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, aPtr, "eq.csa");
        LLVMValueRef bData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, bPtr, "eq.csb");
        LLVMValueRef aLen = LLVMBuildCall2(b->m_builder, b->m_csLenFnType, StrataCStrLenFn(b), &aData, 1, "eq.csla");
        LLVMValueRef bLen = LLVMBuildCall2(b->m_builder, b->m_csLenFnType, StrataCStrLenFn(b), &bData, 1, "eq.cslb");

        LLVMValueRef args[4] = {aData, aLen, bData, bLen};
        StrataStrEqFn(b);
        LLVMValueRef wide = LLVMBuildCall2(b->m_builder, b->m_strEqFnType, b->m_strEqFn, args, 4, "eq.cs");
        eq = LLVMBuildICmp(b->m_builder, LLVMIntNE, wide, LLVMConstInt(I32Ty(b), 0, 0), "eq.cs.b");
    }
    else if (td->structTypeName || TypeDescHasArrayShape(td) || td->isFixedArray)
    {
        LLVMValueRef helper = EnsureEqHelper(b, td, externBoxCtx);
        LLVMTypeRef params[2] = {b->m_ptrTy, b->m_ptrTy};
        LLVMTypeRef fnTy = LLVMFunctionType(I1Ty(b), params, 2, 0);
        LLVMValueRef args[2] = {aPtr, bPtr};
        eq = LLVMBuildCall2(b->m_builder, fnTy, helper, args, 2, "eq.agg");
    }
    else if (TypeDescIsBox(td))
    {
        LLVMValueRef aCell = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, aPtr, "eq.ca");
        LLVMValueRef bCell = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, bPtr, "eq.cb");

        if (externBoxCtx || !td->boxInner)
        {
            /* Host-opaque ^T member, or the degenerate ^string whose cell
               IS a char*: the cell itself decides. */
            eq = LLVMBuildICmp(b->m_builder, LLVMIntEQ, aCell, bCell, "eq.box");
        }
        else
        {
            TypeDesc innerTd = Resolve(b, td->boxInner);
            return EmitBoxCellEq(b, fn, aCell, bCell, &innerTd, TypeDescIsOptional(td), failBB);
        }
    }
    else
    {
        /* Scalars, handles, enums: load both, compare the values (floats
           with fcmp oeq — IEEE-754, matching C). */
        LLVMValueRef av = LLVMBuildLoad2(b->m_builder, td->type, aPtr, "eq.av");
        LLVMValueRef bv = LLVMBuildLoad2(b->m_builder, td->type, bPtr, "eq.bv");
        eq = td->isFloat ? FcmpByName(b->m_builder, "oeq", av, bv)
                         : LLVMBuildICmp(b->m_builder, LLVMIntEQ, av, bv, "eq");
    }

    LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.next");
    LLVMBuildCondBr(b->m_builder, eq, next, failBB);
    LLVMPositionBuilderAtEnd(b->m_builder, next);
    return next;
}

/* Emits the body of the (ptr, ptr) -> i1 equality helper for `td`.
   `externBoxCtx` is true when this walk started inside a host-layout extern
   struct: ^T members there are host-opaque pointers and compare by identity. */
static void EmitEqHelperBody(Builder* b, LLVMValueRef fn, const TypeDesc* td, bool externBoxCtx)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->m_builder);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "entry");
    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "fail");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "done");

    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef bp = LLVMGetParam(fn, 1);
    LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);

    LLVMPositionBuilderAtEnd(b->m_builder, entry);

    if (td->structTypeName)
    {
        /* Struct: member-wise. Each field's compare chains into the next;
           the first mismatch branches to fail. */
        const StructType* st = TypeRegistryFind(&b->m_registry, td->structTypeName);
        bool fieldBoxCtx = externBoxCtx || (st && st->isExtern);

        for (size_t i = 0; st && i < st->fields.count; i++)
        {
            FieldDecl* f = (FieldDecl*)VecGet(&st->fields, i);
            TypeDesc fieldTd = Resolve(b, &f->type);

            LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)i))};
            LLVMValueRef fa = LLVMBuildGEP2(b->m_builder, td->type, a, idxs, 2, "eq.fa");
            LLVMValueRef fb = LLVMBuildGEP2(b->m_builder, td->type, bp, idxs, 2, "eq.fb");
            EmitEqLeaf(b, fn, fa, fb, &fieldTd, fail, fieldBoxCtx);
        }

        LLVMBuildBr(b->m_builder, done);
    }
    else if (td->isFixedArray)
    {
        /* Fixed array: unrolled element-wise compare. */
        TypeDesc elemTd = Resolve(b, td->arrayInner);
        LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

        for (long k = 0; k < td->fixedLength; k++)
        {
            LLVMValueRef idx[1] = {IdxConst(b, (unsigned)k)};
            LLVMValueRef ka = LLVMBuildGEP2(b->m_builder, elemTy, a, idx, 1, "eq.ka");
            LLVMValueRef kb = LLVMBuildGEP2(b->m_builder, elemTy, bp, idx, 1, "eq.kb");
            EmitEqLeaf(b, fn, ka, kb, &elemTd, fail, externBoxCtx);
        }

        LLVMBuildBr(b->m_builder, done);
    }
    else if (TypeDescHasArrayShape(td))
    {
        /* Dynamic array: lengths must match (both-empty is equal), then a
           byte compare for byte-comparable elements or an element-wise
           loop otherwise (never a raw buffer/fat compare). */
        LLVMValueRef aLen = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, a), "eq.ala"));
        LLVMValueRef bLen = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, bp), "eq.alb"));
        LLVMValueRef aData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, a), "eq.ada");
        LLVMValueRef bData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, bp), "eq.adb");

        LLVMBasicBlockRef lenOk = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.len");
        LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, aLen, bLen, "eq.leneq"), lenOk, fail);

        LLVMPositionBuilderAtEnd(b->m_builder, lenOk);
        LLVMBasicBlockRef notEmpty = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.nempty");
        LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntEQ, aLen, zero, "eq.zero"), done, notEmpty);

        LLVMPositionBuilderAtEnd(b->m_builder, notEmpty);
        TypeDesc elemTd = Resolve(b, td->arrayInner);

        if (TypeDescIsByteEq(b, &elemTd))
        {
            LLVMBasicBlockRef bytesDone = EmitEqBytes(
                b, fn, aData, bData, LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTd.type), aLen, "eq.bytes"), fail);
            LLVMPositionBuilderAtEnd(b->m_builder, bytesDone);
            LLVMBuildBr(b->m_builder, done);
        }
        else
        {
            LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

            LLVMBasicBlockRef from = LLVMGetInsertBlock(b->m_builder);
            LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.elem.cond");
            LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "eq.elem.body");

            LLVMBuildBr(b->m_builder, cond);

            LLVMPositionBuilderAtEnd(b->m_builder, cond);
            LLVMValueRef i = LLVMBuildPhi(b->m_builder, I64Ty(b), "eq.i");
            LLVMValueRef iNext = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "eq.i1");
            LLVMBuildCondBr(b->m_builder, LLVMBuildICmp(b->m_builder, LLVMIntULT, i, aLen, "eq.elt"), body, done);

            LLVMPositionBuilderAtEnd(b->m_builder, body);
            LLVMValueRef idx[1] = {i};
            LLVMValueRef ea = LLVMBuildGEP2(b->m_builder, elemTy, aData, idx, 1, "eq.ea");
            LLVMValueRef eb = LLVMBuildGEP2(b->m_builder, elemTy, bData, idx, 1, "eq.eb");
            LLVMBasicBlockRef eqEnd = EmitEqLeaf(b, fn, ea, eb, &elemTd, fail, externBoxCtx);
            LLVMPositionBuilderAtEnd(b->m_builder, eqEnd);
            LLVMBuildBr(b->m_builder, cond);

            LLVMValueRef inVals[2] = {zero, iNext};
            LLVMBasicBlockRef inBlocks[2] = {from, eqEnd};
            LLVMAddIncoming(i, inVals, inBlocks, 2);
        }
    }
    else
    {
        /* Non-aggregate reached directly (defensive): single leaf compare. */
        LLVMBasicBlockRef eqEnd = EmitEqLeaf(b, fn, a, bp, td, fail, externBoxCtx);
        LLVMPositionBuilderAtEnd(b->m_builder, eqEnd);
        LLVMBuildBr(b->m_builder, done);
    }

    LLVMPositionBuilderAtEnd(b->m_builder, done);
    LLVMBuildRet(b->m_builder, LLVMConstInt(I1Ty(b), 1, 0));

    LLVMPositionBuilderAtEnd(b->m_builder, fail);
    LLVMBuildRet(b->m_builder, LLVMConstInt(I1Ty(b), 0, 0));

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->m_builder, savedBlock);
    }
}

LLVMValueRef EnsureEqHelper(Builder* b, const TypeDesc* td, bool externBoxCtx)
{
    const char* baseKey = EqHelperKey(b, td);
    const char* key = externBoxCtx ? arena_format(b->m_arena, "%s!x", baseKey) : baseKey;

    LLVMValueRef fn = (LLVMValueRef)StrMapGet(&b->m_eqHelpers, key);

    if (fn)
    {
        return fn;
    }

    LLVMTypeRef params[2] = {b->m_ptrTy, b->m_ptrTy};
    LLVMTypeRef fnTy = LLVMFunctionType(I1Ty(b), params, 2, 0);
    fn = LLVMAddFunction(b->m_mod, EqHelperName(b, key), fnTy);
    StrMapPut(&b->m_eqHelpers, (char*)key, fn);

    EmitEqHelperBody(b, fn, td, externBoxCtx);

    return fn;
}

/* Parks an already-evaluated aggregate operand in a fresh entry alloca so
   the structural-equality helper can address it byte/field-wise. */
LLVMValueRef SpillEqOperand(Builder* b, Value v)
{
    LLVMValueRef slot = EntryAlloca(b, v.typeDesc.type, "eqtmp");
    LLVMBuildStore(b->m_builder, v.value, slot);
    return slot;
}
