/* Builder: Struct fields, initializers, copy and drop helpers. */
#include "Codegen/Builder/BuilderInternal.h"

/* LLVM member index for logical field `idx` of a struct, mapping through the
   computed layout (explicit fieldoffsets insert padding members). */
unsigned PhysicalFieldIndex(const StructType* st, int idx)
{
    if (st && st->hasLayout && st->physicalIndex && idx >= 0 && (size_t)idx < st->fields.count)
    {
        return (unsigned)st->physicalIndex[idx];
    }

    return (unsigned)(idx < 0 ? 0 : idx);
}

/* `agg` with logical field `idx` set to `value`. A union's fields all overlay byte 0, so they go
   through memory: store the aggregate, store the field over it, reload. */
LLVMValueRef InsertStructField(Builder* b, const StructType* st, LLVMValueRef agg, int idx, LLVMValueRef value)
{
    if (!st || !st->isUnion)
    {
        return LLVMBuildInsertValue(b->m_builder, agg, value, PhysicalFieldIndex(st, idx), "ins");
    }

    LLVMTypeRef aggTy = LLVMTypeOf(agg);
    LLVMValueRef slot = EntryAlloca(b, aggTy, "union");
    LLVMBuildStore(b->m_builder, agg, slot);
    LLVMBuildStore(b->m_builder, value, slot);

    return LLVMBuildLoad2(b->m_builder, aggTy, slot, "union.v");
}

/* Logical field `idx` (of LLVM type `fieldTy`) read out of the aggregate value `agg`. */
LLVMValueRef ExtractStructField(Builder* b, const StructType* st, LLVMValueRef agg, int idx, LLVMTypeRef fieldTy)
{
    if (!st || !st->isUnion)
    {
        return LLVMBuildExtractValue(b->m_builder, agg, PhysicalFieldIndex(st, idx), "m");
    }

    LLVMValueRef slot = EntryAlloca(b, LLVMTypeOf(agg), "union");
    LLVMBuildStore(b->m_builder, agg, slot);

    return LLVMBuildLoad2(b->m_builder, fieldTy, slot, "m");
}

/* Drops the owning fields of a struct at `structPtr`. */
static void EmitDropStructFields(Builder* b, LLVMValueRef structPtr, const char* structName)
{
    /* A field typed as an alias (`struct RA = Res;`) is dropped as the struct it names. */
    structName = TypeRegistryResolveAlias(&b->m_registry, structName);
    const StructType* st = TypeRegistryFind(&b->m_registry, structName);

    if (!st)
    {
        return;
    }

    LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, structName);

    if (!structTy)
    {
        return;
    }

    /* The struct's Drop method, while the value is alive: cleared first, so a re-drop is a no-op. */
    FuncInfo* dropInfo = st->dropFunction ? (FuncInfo*)StrMapGet(&b->m_funcs, st->dropFunction->mangledName) : NULL;
    int aliveIndex = TypeRegistryFieldIndex(&b->m_registry, structName, STRATA_ALIVE_FIELD);

    if (dropInfo && dropInfo->function && aliveIndex >= 0)
    {
        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
        LLVMValueRef aliveIdx[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, aliveIndex))};
        LLVMValueRef aliveAddr = LLVMBuildGEP2(b->m_builder, structTy, structPtr, aliveIdx, 2, "alive");
        LLVMValueRef alive = LLVMBuildLoad2(b->m_builder, i8Ty, aliveAddr, "isalive");
        LLVMValueRef live = LLVMBuildICmp(b->m_builder, LLVMIntNE, alive, LLVMConstInt(i8Ty, 0, 0), "live");

        LLVMBasicBlockRef callBB = NewBb(b, "udrop.call");
        LLVMBasicBlockRef doneBB = NewBb(b, "udrop.done");
        LLVMBuildCondBr(b->m_builder, live, callBB, doneBB);
        b->m_terminated = true;

        PositionAtEnd(b, callBB);
        LLVMBuildStore(b->m_builder, LLVMConstInt(i8Ty, 0, 0), aliveAddr);
        LLVMValueRef dropArgs[2] = {CurrentContext(b), structPtr};
        LLVMBuildCall2(b->m_builder, dropInfo->type, dropInfo->function, dropArgs, 2, "");
        Br(b, doneBB);

        PositionAtEnd(b, doneBB);
    }

    for (size_t j = 0; j < st->fields.count; j++)
    {
        FieldDecl* f = (FieldDecl*)VecGet(&st->fields, j);
        bool fieldOwning = BuilderIsOwningType(b, &f->type);

        /* A plain owning struct held by value is normally rejected (must be
           boxed), but handle it defensively for completeness. */
        bool fieldOwningStruct = TypeRegistryIsOwningStruct(&b->m_registry, f->type.name);

        if (!fieldOwning && !fieldOwningStruct)
        {
            continue;
        }

        LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)j))};
        LLVMValueRef fieldAddr = LLVMBuildGEP2(b->m_builder, structTy, structPtr, idxs, 2, "fd");

        if (fieldOwning)
        {
            TypeDesc fieldTd = Resolve(b, &f->type);
            EmitDropOne(b, fieldAddr, fieldTd);
        }
        else
        {
            EmitDropStructFields(b, fieldAddr, f->type.name);
        }
    }
}

/* Builds a drop fn body: `if (!p) return;` then EmitDropStructFields(p). May
   run mid-emission of another function (a ^T drop can be reached anywhere),
   so the enclosing build context is saved and restored around it. */
static void EmitStructDropFnBody(Builder* b, LLVMValueRef fn, const char* structName)
{
    LLVMValueRef savedCurFn = b->m_curFn;
    LLVMBasicBlockRef savedEntryBlock = b->m_entryBlock;
    LLVMValueRef savedEntryAllocaPt = b->m_entryAllocaPt;
    LLVMValueRef savedGlobalsPtr = b->m_curGlobalsPtr;
    bool savedTerminated = b->m_terminated;
    LLVMBasicBlockRef savedInsertBlock = LLVMGetInsertBlock(b->m_builder);

    b->m_curFn = fn;
    b->m_curGlobalsPtr = LLVMGetParam(fn, 1); /* the dropping function's context, for Drop methods */

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "entry");
    PositionAtEnd(b, entry);
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;

    LLVMValueRef p = LLVMGetParam(fn, 0);

    LLVMBasicBlockRef nullBB = NewBb(b, "sdrop.null");
    LLVMBasicBlockRef doBB = NewBb(b, "sdrop.do");

    LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, p, LLVMConstNull(b->m_ptrTy), "isnull");
    LLVMBuildCondBr(b->m_builder, isNull, nullBB, doBB);
    b->m_terminated = true;

    PositionAtEnd(b, nullBB);
    LLVMBuildRetVoid(b->m_builder);
    b->m_terminated = true;

    PositionAtEnd(b, doBB);

    EmitDropStructFields(b, p, structName);

    if (!b->m_terminated)
    {
        LLVMBuildRetVoid(b->m_builder);
        b->m_terminated = true;
    }

    b->m_curFn = savedCurFn;
    b->m_entryBlock = savedEntryBlock;
    b->m_entryAllocaPt = savedEntryAllocaPt;
    b->m_curGlobalsPtr = savedGlobalsPtr;
    b->m_terminated = savedTerminated;

    if (savedInsertBlock)
    {
        LLVMPositionBuilderAtEnd(b->m_builder, savedInsertBlock);
    }
}

/* Returns (creating if needed) a per-struct drop fn for `structName`'s owning
   fields, used instead of inlining at ^T drop sites. A self-referential/
   mutually recursive owning struct would otherwise recurse without bound at
   IR-gen time; a real call terminates via the callee's null check. */
LLVMValueRef GetOrCreateStructDropFn(Builder* b, const char* structName)
{
    LLVMValueRef existing = (LLVMValueRef)StrMapGet(&b->m_dropFns, structName);

    if (existing)
    {
        return existing;
    }

    LLVMTypeRef params[2] = {b->m_ptrTy, b->m_ptrTy}; /* (value, context) */
    LLVMTypeRef fnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 2, 0);
    char* fnName = arena_format(b->m_arena, "__strata_drop_%s", structName);
    LLVMValueRef fn = LLVMAddFunction(b->m_mod, fnName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    /* Register before emitting the body: a self-referential or mutually
       recursive struct's own drop body will look this function back up
       (cache hit) instead of triggering another EmitStructDropFnBody call. */
    StrMapPut(&b->m_dropFns, structName, (void*)fn);

    EmitStructDropFnBody(b, fn, structName);

    return fn;
}

/* GEP to a module global's field inside the current function's per-instance
   globals struct (see BuilderBuild's "instanced globals" pass). Only
   reachable when `name` is a storage-backed global, which implies
   b->m_hasInstancedGlobals and therefore a bound b->m_curGlobalsPtr in any
   function whose body can reference it (extern functions have no body). */
LLVMValueRef GlobalFieldPtr(Builder* b, const char* name)
{
    void* raw = StrMapGet(&b->m_globalFieldIndex, name);
    unsigned index = (unsigned)((uintptr_t)raw - 1);
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, index)};
    return LLVMBuildGEP2(b->m_builder, b->m_globalsStructTy, b->m_curGlobalsPtr, idx, 2, "gfld");
}

/* Deep-copies an owning-struct VALUE field-by-field: owning fields recurse
   through EmitCopyValue, plain fields bitwise. */
Value EmitStructValueCopy(Builder* b, Value src, TypeDesc td)
{
    const StructType* st = TypeRegistryFind(&b->m_registry, td.structTypeName);
    LLVMTypeRef structTy = st ? (LLVMTypeRef)StrMapGet(&b->m_structTypes, td.structTypeName) : NULL;

    if (!st || !structTy)
    {
        return src;
    }

    LLVMValueRef srcSlot = EntryAlloca(b, td.type, "cpss");
    LLVMBuildStore(b->m_builder, src.value, srcSlot);
    LLVMValueRef dstSlot = EntryAlloca(b, td.type, "cpsd");

    /* The cached copy fn is a real call: self-referential owning structs
       (a `Node? next` field) recurse at runtime, not at IR-gen time. */
    LLVMTypeRef copyParamTys[2] = {b->m_ptrTy, b->m_ptrTy};
    LLVMTypeRef copyFnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), copyParamTys, 2, 0);
    LLVMValueRef copyFn = GetOrCreateStructCopyFn(b, td.structTypeName);
    LLVMValueRef copyArgs[2] = {dstSlot, srcSlot};
    LLVMBuildCall2(b->m_builder, copyFnTy, copyFn, copyArgs, 2, "");

    return ValueMake(LLVMBuildLoad2(b->m_builder, td.type, dstSlot, "cpS"), td);
}

/* Copies an owning struct value field-by-field from srcPtr into dstPtr.
   Owning fields recurse through EmitCopyValue; a self-referential owning
   struct terminates because struct/box field copies go through the cached
   per-struct copy fn (a real call), never inline expansion. */

/* Copies `srcPtr`'s struct fields into `dstPtr`. Owning fields recurse
   through EmitCopyValue (whose struct/box branches bottom out in the cached
   copy fn — a real call), plain fields bitwise. */
static void EmitStructCopyInto(Builder* b, LLVMValueRef dstPtr, LLVMValueRef srcPtr, const char* structName)
{
    const StructType* st = TypeRegistryFind(&b->m_registry, structName);
    LLVMTypeRef structTy = st ? (LLVMTypeRef)StrMapGet(&b->m_structTypes, structName) : NULL;

    if (!st || !structTy)
    {
        return;
    }

    for (size_t j = 0; j < st->fields.count; j++)
    {
        FieldDecl* f = (FieldDecl*)VecGet(&st->fields, j);
        TypeDesc fieldTd = Resolve(b, &f->type);
        LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)j))};
        LLVMValueRef srcField = LLVMBuildGEP2(b->m_builder, structTy, srcPtr, idxs, 2, "csf");
        LLVMValueRef dstField = LLVMBuildGEP2(b->m_builder, structTy, dstPtr, idxs, 2, "cdf");

        if (BuilderIsOwningValue(b, &f->type))
        {
            Value fv = ValueMake(LLVMBuildLoad2(b->m_builder, fieldTd.type, srcField, "fv"), fieldTd);
            Value copied = EmitCopyValue(b, fv, fieldTd);
            LLVMBuildStore(b->m_builder, copied.value, dstField);
        }
        else
        {
            LLVMValueRef loaded = LLVMBuildLoad2(b->m_builder, fieldTd.type, srcField, "fv");
            LLVMBuildStore(b->m_builder, loaded, dstField);
        }
    }
}

/* Returns (creating if needed) a per-struct deep-copy fn
   `void __strata_copy_<T>(dst, src)` — the call-based form of
   EmitStructCopyInto that self-referential structs can recurse through at
   runtime (the same escape hatch the drop fns use). */

/* Builds (once) `void __strata_copy_<T>(dst, src)` around EmitStructCopyInto.
   Registered in the cache BEFORE the body is emitted, so a self-referential
   owning struct's own copy body looks the function back up (cache hit) and
   recurses with a real call at runtime. May run mid-emission of another
   function, so the enclosing build context is saved and restored. */
LLVMValueRef GetOrCreateStructCopyFn(Builder* b, const char* structName)
{
    LLVMValueRef existing = (LLVMValueRef)StrMapGet(&b->m_copyFns, structName);

    if (existing)
    {
        return existing;
    }

    LLVMTypeRef params[2] = {b->m_ptrTy, b->m_ptrTy};
    LLVMTypeRef fnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 2, 0);
    char* fnName = arena_format(b->m_arena, "__strata_copy_%s", structName);
    LLVMValueRef fn = LLVMAddFunction(b->m_mod, fnName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    StrMapPut(&b->m_copyFns, structName, (void*)fn);

    LLVMValueRef savedCurFn = b->m_curFn;
    LLVMBasicBlockRef savedEntryBlock = b->m_entryBlock;
    LLVMValueRef savedEntryAllocaPt = b->m_entryAllocaPt;
    bool savedTerminated = b->m_terminated;
    LLVMBasicBlockRef savedInsertBlock = LLVMGetInsertBlock(b->m_builder);

    b->m_curFn = fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "entry");
    PositionAtEnd(b, entry);
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;

    EmitStructCopyInto(b, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1), structName);

    LLVMBuildRetVoid(b->m_builder);
    b->m_terminated = true;

    b->m_curFn = savedCurFn;
    b->m_entryBlock = savedEntryBlock;
    b->m_entryAllocaPt = savedEntryAllocaPt;
    b->m_terminated = savedTerminated;

    if (savedInsertBlock)
    {
        LLVMPositionBuilderAtEnd(b->m_builder, savedInsertBlock);
    }

    return fn;
}

bool FieldInitMovesSource(Builder* b, TypeDesc fieldTd, TypeDesc valueTd, const Node* valueNode)
{
    if (valueNode->kind == NodeStrLiteral || valueNode->kind == NodeArrayInit)
    {
        return false;
    }

    return ((TypeDescIsBox(&fieldTd) || fieldTd.isString) && (TypeDescIsBox(&valueTd) || valueTd.isString))
           || (TypeDescHasArrayShape(&fieldTd) && TypeDescHasArrayShape(&valueTd))
           || (fieldTd.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, fieldTd.structTypeName)
               && valueTd.structTypeName && strcmp(fieldTd.structTypeName, valueTd.structTypeName) == 0);
}

Value EmitStructInit(Builder* b, StructInitExpr* n)
{
    TypeName tn = MakeTypeName(b, n->typeName);
    TypeDesc typeDesc = Resolve(b, &tn);

    const StructType* st = TypeRegistryFind(&b->m_registry, n->typeName);

    /* Fields the literal leaves out keep their declared defaults. */
    LLVMValueRef agg = DefaultValueOf(b, &tn, typeDesc);

    size_t positionalIndex = 0;

    for (size_t i = 0; i < n->fields.count; i++)
    {
        StructInitField* field = (StructInitField*)VecGet(&n->fields, i);

        size_t idx = 0;

        if (!field->name || field->name[0] == '\0')
        {
            idx = positionalIndex++;
        }
        else
        {
            int named = TypeRegistryFieldIndex(&b->m_registry, n->typeName, field->name);

            if (named < 0)
            {
                continue;
            }

            idx = (size_t)named;
        }

        if (!st || idx >= st->fields.count)
        {
            continue;
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, idx);
        TypeDesc fieldTd = Resolve(b, &fieldDecl->type);

        if (fieldTd.isFixedArray && field->value->kind == NodeArrayInit)
        {
            /* Fixed-size array field from a braced literal: the flat list
                carries row-major elements (sema placed nested rows at their
                offsets); NULL entries are holes that stay zero. */
            ArrayInitExpr* ai = (ArrayInitExpr*)field->value;
            TypeDesc elemTd = Resolve(b, ai->elementType);

            LLVMValueRef arr = DefaultValueOf(b, &fieldDecl->type, fieldTd);

            for (size_t k = 0; k < ai->elements.count; k++)
            {
                Node* elem = (Node*)VecGet(&ai->elements, k);

                if (!elem)
                {
                    continue;
                }

                Value ev = Coerce(b, EmitExpr(b, elem), elemTd);
                arr = InsertFixedElem(b, arr, ev.value, &fieldDecl->type, (long)k);
            }

            agg = InsertStructField(b, st, agg, (int)idx, arr);
            continue;
        }

        Value rawField = EmitExpr(b, field->value);
        Value fieldValue;

        if (fieldTd.isString && field->value->kind == NodeStrLiteral)
        {
            /* string field from a literal -> heap copy (safe to free). */
            LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, rawField.value, 0, "fslit.p");
            LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, rawField.value, 1, "fslit.l");
            fieldValue = ValueMake(BuildOwnedStringFat(b, sp, sl), fieldTd);
        }
        else if (TypeDescIsBox(&fieldTd) && fieldTd.boxInner
                 && !(TypeDescIsBox(&rawField.typeDesc) && rawField.typeDesc.boxInner
                      && strcmp(rawField.typeDesc.boxInner->name, fieldTd.boxInner->name) == 0))
        {
            /* ^T field from a non-^T value (bare T, string literal,
               string variable into ^string): allocate a T slot, construct
               the owned inner value, store it. Same pattern as a top-level
               ^T init. */
            LLVMValueRef heap = EmitBoxCell(b, fieldTd.boxInner, rawField, field->value, "fieldbox");
            fieldValue = ValueMake(heap, fieldTd);
        }
        else if (BoxIntoInlineOwning(b, fieldTd, rawField.typeDesc))
        {
            agg = InsertStructField(b, st, agg, (int)idx, TakeBoxContents(b, rawField, field->value).value);
            continue;
        }
        else
        {
            fieldValue = Coerce(b, rawField, fieldTd);
        }

        agg = InsertStructField(b, st, agg, (int)idx, fieldValue.value);

        /* If an owning field was moved from an owning lvalue source, null
           the source so its scope-exit drop is a no-op. */
        if (FieldInitMovesSource(b, fieldTd, rawField.typeDesc, field->value))
        {
            NullMovedSource(b, field->value);
        }
    }

    return ValueMake(agg, typeDesc);
}

/* A packed struct places fields at offsets its types' natural alignment would not allow, so a load or store that
   goes through one of its fields cannot assume the usual alignment: a `float4` at offset 5 read with an aligned
   vector instruction faults. Marking those accesses align 1 makes the backend pick the unaligned forms. */
void MarkPackedFieldAccessesUnaligned(LLVMModuleRef mod)
{
    for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn; fn = LLVMGetNextFunction(fn))
    {
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb))
        {
            for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst; inst = LLVMGetNextInstruction(inst))
            {
                bool isLoad = LLVMIsALoadInst(inst) != NULL;
                bool isStore = LLVMIsAStoreInst(inst) != NULL;

                if (!isLoad && !isStore)
                {
                    continue;
                }

                LLVMValueRef ptr = LLVMGetOperand(inst, isLoad ? 0 : 1);

                if (ptr && LLVMIsAGetElementPtrInst(ptr)
                    && LLVMGetTypeKind(LLVMGetGEPSourceElementType(ptr)) == LLVMStructTypeKind
                    && LLVMIsPackedStruct(LLVMGetGEPSourceElementType(ptr)))
                {
                    LLVMSetAlignment(inst, 1);
                }
            }
        }
    }
}
