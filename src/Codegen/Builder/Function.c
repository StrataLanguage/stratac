/* Builder: Function declaration, definition and AOT entry point. */
#include "Codegen/Builder/BuilderInternal.h"

#define STRATA_AOT_MAIN_SYMBOL "__strata_main"

/* An AOT module's `int main()` (or `void main()`) is the program entry: its Strata body is emitted
   as __strata_main and EmitAotEntryPoint wraps it in a real C `main` that owns the context. */
bool IsAotEntryPoint(const Builder* b, const FunctionDecl* f)
{
    if (b->m_jitMode || f->isExtern || f->isExport || f->fromImpl || !f->body || f->params.count != 0
        || strcmp(f->mangledName, "main") != 0)
    {
        return false;
    }

    PrimitiveType ret = GetPrimitiveType(f->returnType.name);

    return ret == PrimInt || ret == PrimVoid;
}

void DeclareFunction(Builder* b, const FunctionDecl* f)
{
    /* An `extern<T>` itself is never called (sema redirects calls to instantiations), but the
       host binds its name, so the JIT needs its slot. */
    if (f->typeParam)
    {
        if (b->m_jitMode)
        {
            ExternSlot(b, f->name);
        }

        return;
    }

    FuncInfo* info = (FuncInfo*)arena_alloc(b->m_arena, sizeof(FuncInfo));

    bool hasReturnParam = FunctionHasReturnParam(f);

    info->returnType = Resolve(b, &f->returnType);

    /* Remember the semantic return type before widening so extern call
       sites can narrow the (partially-defined) register back down. */
    info->retSemanticTy = info->returnType.type;
    info->retSemanticUnsigned = TypeDescIsUnsigned(&info->returnType);
    info->returnType.type = WidenRetType(b, info->returnType.type);

    /* An extern `string` (or alias-of-string) return crosses the host
       boundary as a NUL-terminated char*: the caller copies it into a fresh
       owned buffer. `string?` (the maybe-empty fat) stays banned in sema. */
    info->externStringReturn = f->isExtern && !hasReturnParam && !TypeNameIsOptional(&f->returnType)
                               && TypeIsString(&b->m_registry, f->returnType.name);

    size_t pcount = f->params.count;
    info->paramByPtr = (bool*)arena_alloc(b->m_arena, pcount * sizeof(bool));
    info->paramByPtrCount = pcount;

    /* Every non-extern function gets ONE hidden leading LLVM parameter (a
       pointer to the per-instance globals struct) that never appears at the
       Strata source level: invisible in `f->params`, added only to the
       compiled signature here and forwarded automatically by EmitCall. It is
       there even when the module has no instanced globals (the context is
       then NULL), so hosts never branch on the ABI. */
    info->hasHiddenCtxParam = !f->isExtern && !f->isExport;
    size_t ctxOffset = info->hasHiddenCtxParam ? 1 : 0;

    info->hasTypeDescParam = f->genericOf != NULL;
    size_t typeDescCount = info->hasTypeDescParam ? 1 : 0;

    LLVMTypeRef* params = NULL;

    if (pcount + ctxOffset + typeDescCount > 0)
    {
        params = (LLVMTypeRef*)arena_alloc(b->m_arena, (pcount + ctxOffset + typeDescCount) * sizeof(LLVMTypeRef));
    }

    if (info->hasHiddenCtxParam)
    {
        params[0] = b->m_ptrTy;
    }

    for (size_t i = 0; i < pcount; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);

        /* Structs pass BY VALUE: the callee gets its own copy (an explicit
           `ref` still shares the caller's storage). Sema makes extern struct
           params spell out `ref`, since the host C ABI contract is `T*`. */
        bool byPtr = p->mod != ModNone || BuilderIsOwningType(b, &p->type);

        // Extern strings cross as char*: plain passes its buffer, optional passes raw ptr.
        bool optionalString
            = TypeNameIsOptional(&p->type) && p->type.inner && TypeIsString(&b->m_registry, p->type.inner->name);
        bool externStringParam = f->isExtern && (TypeIsString(&b->m_registry, p->type.name) || optionalString);

        if (byPtr && externStringParam)
        {
            byPtr = false;
        }

        // Extern box/optional params cross as the pointer itself.
        if (byPtr && f->isExtern && (TypeNameIsBox(&p->type) || TypeNameIsOptional(&p->type)))
        {
            byPtr = false;
        }

        info->paramByPtr[i] = byPtr;

        params[i + ctxOffset] = byPtr ? b->m_ptrTy : (externStringParam ? b->m_ptrTy : Resolve(b, &p->type).type);
    }

    // A function with a `return` param == void ret + out-pointer. An extern
    // `string` return crosses as char* (caller copies it into an owned fat).
    LLVMTypeRef abiRetType = hasReturnParam ? LLVMVoidTypeInContext(b->m_ctx)
                                            : (info->externStringReturn ? b->m_ptrTy : info->returnType.type);

    if (info->hasTypeDescParam)
    {
        params[pcount + ctxOffset] = b->m_ptrTy;
    }

    info->type
        = LLVMFunctionType(abiRetType, params, (unsigned)(pcount + ctxOffset + typeDescCount), f->isCVararg ? 1 : 0);

    /* A delegate is a call signature, not a symbol: calls go through a pointer value. */
    if (f->isDelegate)
    {
        info->function = NULL;
        StrMapPut(&b->m_funcs, f->mangledName, info);
        return;
    }

    if (b->m_jitMode && f->isExtern)
    {
        LLVMValueRef slot = ExternSlot(b, f->name);

        /* Calls to an instantiation name it by its mangled `Name<T>`. */
        if (f->genericOf)
        {
            StrMapPut(&b->m_externSlots, f->mangledName, (void*)slot);
        }

        info->function = NULL;
    }
    else if (f->genericOf)
    {
        /* Every instantiation has the same ABI (T crosses as a pointer), so all share one declaration. */
        LLVMValueRef existing = LLVMGetNamedFunction(b->m_mod, f->name);
        info->function = existing ? existing : LLVMAddFunction(b->m_mod, f->name, info->type);
    }
    else
    {
        /* Externs and `@export`s keep their C names; the AOT entry point is renamed so the
           synthesized C `main` (EmitAotEntryPoint) can wrap it. */
        const char* symbol = (f->isExtern || f->isExport) ? f->mangledName
                             : IsAotEntryPoint(b, f)      ? ExportedSymbolName(b, STRATA_AOT_MAIN_SYMBOL)
                                                          : ExportedSymbolName(b, f->mangledName);
        info->function = LLVMAddFunction(b->m_mod, symbol, info->type);
    }

    StrMapPut(&b->m_funcs, f->mangledName, info);
}

void DefineFunction(Builder* b, const FunctionDecl* f)
{
    if (!f->body)
    {
        return;
    }

    StrMapClear(&b->m_symbols);
    b->m_terminated = false;
    b->m_curRet = Resolve(b, &f->returnType);
    b->m_curRetAbi = WidenRetType(b, b->m_curRet.type);
    b->m_loops.count = 0;
    b->m_owningLocals.count = 0;
    b->m_scopes.count = 0;
    b->m_symDecls.count = 0;
    ResetTemps(b);

    FuncInfo* info = (FuncInfo*)StrMapGet(&b->m_funcs, f->mangledName);
    b->m_curFn = info ? info->function : NULL;

    bool hasHiddenCtx = info && info->hasHiddenCtxParam;
    unsigned paramBase = hasHiddenCtx ? 1u : 0u;
    b->m_curGlobalsPtr = hasHiddenCtx ? LLVMGetParam(b->m_curFn, 0) : NULL;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "entry");
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;
    LLVMPositionBuilderAtEnd(b->m_builder, entry);

    /* An `@export` body has no context parameter: it uses the module's newest context. */
    if (f->isExport)
    {
        b->m_curGlobalsPtr = b->m_exportCtxGlobal
                                 ? LLVMBuildLoad2(b->m_builder, b->m_ptrTy, b->m_exportCtxGlobal, "exportctx")
                                 : LLVMConstNull(b->m_ptrTy);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        TypeDesc typeDesc = Resolve(b, &p->type);

        const char* leafName = TypeRegistryResolveAlias(&b->m_registry, p->type.name);
        bool structVal
            = TypeRegistryIsUserType(&b->m_registry, leafName) && !TypeRegistryIsOpaque(&b->m_registry, leafName);
        bool boxParam = BuilderIsOwningType(b, &p->type);
        /* An owning struct passed BY VALUE (non-ref, non-extern): the callee
           owns its copy's fields and drops them at return. */
        bool owningStructParam = structVal && !boxParam && TypeRegistryIsOwningStruct(&b->m_registry, leafName);

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));

        if (p->mod != ModNone || (f->isExtern && structVal) || boxParam)
        {
            sym->value = LLVMGetParam(b->m_curFn, (unsigned)(i + paramBase));
            sym->typeDesc = typeDesc;

            /* A `ref T... rest` with non-owning elements collects pointers to
               the source arguments; element access derefs the slot. */
            if (p->isVarargRest && p->mod == ModRef)
            {
                const TypeName* elem = TypeNameArrayElem(&p->type);

                if (elem && !BuilderIsOwningType(b, elem))
                {
                    TypeDescSetAliasedArray(&sym->typeDesc, true);
                }
            }

            /* An owned (non-ref) box/array parameter is consumed: freed at return.
               A vararg rest array is stack-backed: its owning ELEMENTS are
               dropped, but the caller's stack buffer is not freed. */
            if (boxParam && p->mod == ModNone)
            {
                OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
                ol->slot = sym->value;
                ol->td = typeDesc;
                ol->stackBuffer = p->isVarargRest;
                VecPush(&b->m_owningLocals, ol);
            }
        }
        else
        {
            LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "arg");
            LLVMBuildStore(b->m_builder, LLVMGetParam(b->m_curFn, (unsigned)(i + paramBase)), slot);

            sym->value = slot;
            sym->typeDesc = typeDesc;

            if (owningStructParam)
            {
                OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
                ol->slot = slot;
                ol->td = typeDesc;
                ol->stackBuffer = false;
                VecPush(&b->m_owningLocals, ol);
            }
        }

        StrMapPut(&b->m_symbols, p->name, sym);
    }

    Block* blk = (Block*)f->body;

    PushScope(b);
    SymPush(b);

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        Node* s = (Node*)VecGet(&blk->statements, i);
        EmitStmt(b, s);

        if (b->m_terminated)
        {
            break;
        }
    }

    if (!b->m_terminated)
    {
        RunDefers(b, TopScope(b));
    }

    SymPop(b);
    PopScope(b);

    if (!b->m_terminated)
    {
        EmitDrops(b, 0);

        if (TypeDescIsVoid(&b->m_curRet))
        {
            LLVMBuildRetVoid(b->m_builder);
        }
        else
        {
            LLVMBuildRet(b->m_builder, LLVMConstNull(b->m_curRetAbi));
        }
    }
}

/* int main(int argc, char** argv): create the context, run the Strata entry, destroy the context. */
void EmitAotEntryPoint(Builder* b, const FunctionDecl* entry)
{
    FuncInfo* info = (FuncInfo*)StrMapGet(&b->m_funcs, entry->mangledName);
    LLVMValueRef createFn = LLVMGetNamedFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_create"));
    LLVMValueRef destroyFn = LLVMGetNamedFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_destroy"));

    if (!info || !info->function || !createFn || !destroyFn)
    {
        return;
    }

    LLVMTypeRef params[2] = {I32Ty(b), b->m_ptrTy};
    LLVMTypeRef mainTy = LLVMFunctionType(I32Ty(b), params, 2, 0);
    LLVMValueRef mainFn = LLVMAddFunction(b->m_mod, "main", mainTy);

    LLVMBuilderRef builder = LLVMCreateBuilderInContext(b->m_ctx);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(b->m_ctx, mainFn, "entry"));

    LLVMTypeRef createTy = LLVMFunctionType(b->m_ptrTy, NULL, 0, 0);
    LLVMValueRef ctx = LLVMBuildCall2(builder, createTy, createFn, NULL, 0, "ctx");

    LLVMValueRef callArgs[1] = {ctx};
    LLVMValueRef result = LLVMBuildCall2(builder, info->type, info->function, callArgs, 1, "");
    LLVMValueRef exitCode
        = GetPrimitiveType(entry->returnType.name) == PrimVoid ? LLVMConstInt(I32Ty(b), 0, 0) : result;

    LLVMTypeRef destroyParams[1] = {b->m_ptrTy};
    LLVMTypeRef destroyTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), destroyParams, 1, 0);
    LLVMBuildCall2(builder, destroyTy, destroyFn, callArgs, 1, "");

    LLVMBuildRet(builder, exitCode);
    LLVMDisposeBuilder(builder);
}
