// Builder: Function declaration, definition and AOT entry point.
#include "Codegen/Builder/BuilderInternal.h"

#define STRATA_AOT_MAIN_SYMBOL "__strata_main"

// A function the C `main` would be: a plain, defined `main` in an AOT module.
bool IsAotMain(const Builder* b, const FunctionDecl* f)
{
    return !b->jitMode && !f->isExtern && !f->isExport && !f->fromImpl && f->body
           && strcmp(f->mangledName, "main") == 0;
}

// Parameter `i` of an entry point is spelled `int argc` (i == 0) or `ptr argv` (i == 1), taken by value.
static bool IsEntryParam(const FunctionDecl* f, size_t i)
{
    const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&f->params, i);
    const TypeName* t = &p->type;

    if (p->mod != ModNone || p->isVarargRest || TypeNameHasArrayShape(t) || TypeNameIsBox(t) || TypeNameIsOptional(t)
        || !t->name)
    {
        return false;
    }

    return i == 0 ? GetPrimitiveType(t->name) == PrimInt : strcmp(t->name, STRATA_PTR_TYPE) == 0;
}

// An AOT module's `int main()`, `int main(int argc)` or `int main(int argc, ptr argv)` (or the `void` forms) is
// the program entry: its Strata body is emitted as __strata_main and EmitAotEntryPoint wraps it in a real C
// `main` that owns the context and forwards argc and argv.
bool IsAotEntryPoint(const Builder* b, const FunctionDecl* f)
{
    if (!IsAotMain(b, f) || f->params.count > 2 || f->isVariadic)
    {
        return false;
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        if (!IsEntryParam(f, i))
        {
            return false;
        }
    }

    PrimitiveType ret = GetPrimitiveType(f->returnType.name);

    return ret == PrimInt || ret == PrimVoid;
}

void DeclareFunction(Builder* b, const FunctionDecl* f)
{
    // An `extern<T>` itself is never called (sema redirects calls to instantiations), but the
    // host binds its name, so the JIT needs its slot.
    if (f->typeParam)
    {
        if (b->jitMode)
        {
            ExternSlot(b, f->name);
        }

        return;
    }

    FuncInfo* info = (FuncInfo*)ArenaAlloc(b->arena, sizeof(FuncInfo));

    bool hasReturnParam = FunctionHasReturnParam(f);

    info->returnType = Resolve(b, &f->returnType);

    // Remember the semantic return type before widening so extern call
    // sites can narrow the (partially-defined) register back down.
    info->retSemanticTy = info->returnType.type;
    info->retSemanticUnsigned = TypeDescIsUnsigned(&info->returnType);
    info->returnType.type = WidenRetType(b, info->returnType.type);

    // An extern `string` (or alias-of-string) return crosses the host
    // boundary as a NUL-terminated char*: the caller copies it into a fresh
    // owned buffer. `string?` (the maybe-empty fat) stays banned in sema.
    info->externStringReturn = f->isExtern && !hasReturnParam && !TypeNameIsOptional(&f->returnType)
                               && TypeIsString(&b->registry, f->returnType.name);

    size_t pcount = f->params.count;
    info->paramByPtr = (bool*)ArenaAlloc(b->arena, pcount * sizeof(bool));
    info->paramByPtrCount = pcount;

    // Every non-extern function gets ONE hidden leading LLVM parameter (a
    // pointer to the per-instance globals struct) that never appears at the
    // Strata source level: invisible in `f->params`, added only to the
    // compiled signature here and forwarded automatically by EmitCall. It is
    // there even when the module has no instanced globals (the context is
    // then NULL), so hosts never branch on the ABI.
    info->hasHiddenCtxParam = !f->isExtern && !f->isExport;
    size_t ctxOffset = info->hasHiddenCtxParam ? 1 : 0;

    info->hasTypeDescParam = f->genericOf != NULL;
    size_t typeDescCount = info->hasTypeDescParam ? 1 : 0;

    LLVMTypeRef* params = NULL;

    if (pcount + ctxOffset + typeDescCount > 0)
    {
        params = (LLVMTypeRef*)ArenaAlloc(b->arena, (pcount + ctxOffset + typeDescCount) * sizeof(LLVMTypeRef));
    }

    if (info->hasHiddenCtxParam)
    {
        params[0] = b->ptrTy;
    }

    for (size_t i = 0; i < pcount; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);

        // Structs pass BY VALUE: the callee gets its own copy (an explicit
        // `ref` still shares the caller's storage). Sema makes extern struct
        // params spell out `ref`, since the host C ABI contract is `T*`.
        bool byPtr = p->mod != ModNone || BuilderIsOwningType(b, &p->type);

        // Extern strings cross as char*: plain passes its buffer, optional passes raw ptr.
        bool optionalString
            = TypeNameIsOptional(&p->type) && p->type.inner && TypeIsString(&b->registry, p->type.inner->name);
        bool externStringParam = f->isExtern && (TypeIsString(&b->registry, p->type.name) || optionalString);

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

        params[i + ctxOffset] = byPtr ? b->ptrTy : (externStringParam ? b->ptrTy : Resolve(b, &p->type).type);
    }

    // A function with a `return` param == void ret + out-pointer. An extern
    // `string` return crosses as char* (caller copies it into an owned fat).
    LLVMTypeRef abiRetType = hasReturnParam ? LLVMVoidTypeInContext(b->ctx)
                                            : (info->externStringReturn ? b->ptrTy : info->returnType.type);

    if (info->hasTypeDescParam)
    {
        params[pcount + ctxOffset] = b->ptrTy;
    }

    info->type
        = LLVMFunctionType(abiRetType, params, (unsigned)(pcount + ctxOffset + typeDescCount), f->isCVararg ? 1 : 0);

    // A delegate is a call signature, not a symbol: calls go through a pointer value.
    if (f->isDelegate)
    {
        info->function = NULL;
        StrMapPut(&b->funcs, f->mangledName, info);
        return;
    }

    if (b->jitMode && f->isExtern)
    {
        LLVMValueRef slot = ExternSlot(b, f->name);

        // Calls to an instantiation name it by its mangled `Name<T>`.
        if (f->genericOf)
        {
            StrMapPut(&b->externSlots, f->mangledName, (void*)slot);
        }

        info->function = NULL;
    }
    else if (f->genericOf)
    {
        // Every instantiation has the same ABI (T crosses as a pointer), so all share one declaration.
        LLVMValueRef existing = LLVMGetNamedFunction(b->mod, f->name);
        info->function = existing ? existing : LLVMAddFunction(b->mod, f->name, info->type);
    }
    else
    {
        // Externs and `@export`s keep their C names; the AOT entry point is renamed so the
        // synthesized C `main` (EmitAotEntryPoint) can wrap it.
        const char* symbol = (f->isExtern || f->isExport) ? f->mangledName
                             : IsAotEntryPoint(b, f)      ? ExportedSymbolName(b, STRATA_AOT_MAIN_SYMBOL)
                                                          : ExportedSymbolName(b, f->mangledName);

        // The same extern may be declared in several files: they name one C symbol, so reuse its declaration
        // (a second LLVMAddFunction would be renamed `name.1` and fail to link).
        LLVMValueRef existing = f->isExtern ? LLVMGetNamedFunction(b->mod, symbol) : NULL;
        info->function = existing ? existing : LLVMAddFunction(b->mod, symbol, info->type);
    }

    StrMapPut(&b->funcs, f->mangledName, info);
}

void DefineFunction(Builder* b, const FunctionDecl* f)
{
    if (!f->body)
    {
        return;
    }

    StrMapClear(&b->symbols);
    b->terminated = false;
    b->curDecl = f;
    b->curRet = Resolve(b, &f->returnType);
    b->curRetAbi = WidenRetType(b, b->curRet.type);
    b->loops.count = 0;
    b->owningLocals.count = 0;
    b->scopes.count = 0;
    b->symDecls.count = 0;
    ResetTemps(b);

    FuncInfo* info = (FuncInfo*)StrMapGet(&b->funcs, f->mangledName);
    b->curFn = info ? info->function : NULL;

    bool hasHiddenCtx = info && info->hasHiddenCtxParam;
    unsigned paramBase = hasHiddenCtx ? 1u : 0u;
    b->curGlobalsPtr = hasHiddenCtx ? LLVMGetParam(b->curFn, 0) : NULL;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->ctx, b->curFn, "entry");
    b->entryBlock = entry;
    b->entryAllocaPt = NULL;
    LLVMPositionBuilderAtEnd(b->builder, entry);

    // An `@export` body has no context parameter: it uses the module's newest context.
    if (f->isExport)
    {
        b->curGlobalsPtr = b->exportCtxGlobal ? LLVMBuildLoad2(b->builder, b->ptrTy, b->exportCtxGlobal, "exportctx")
                                              : LLVMConstNull(b->ptrTy);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        TypeDesc typeDesc = Resolve(b, &p->type);

        const char* leafName = TypeRegistryResolveAlias(&b->registry, p->type.name);
        bool structVal
            = TypeRegistryIsUserType(&b->registry, leafName) && !TypeRegistryIsOpaque(&b->registry, leafName);
        bool boxParam = BuilderIsOwningType(b, &p->type);
        // An owning struct passed BY VALUE (non-ref, non-extern): the callee
        // owns its copy's fields and drops them at return.
        bool owningStructParam = structVal && !boxParam && TypeRegistryIsOwningStruct(&b->registry, leafName);

        Value* sym = (Value*)ArenaAlloc(b->arena, sizeof(Value));

        if (p->mod != ModNone || (f->isExtern && structVal) || boxParam)
        {
            sym->value = LLVMGetParam(b->curFn, (unsigned)(i + paramBase));
            sym->typeDesc = typeDesc;

            // A `ref T... rest` with non-owning elements collects pointers to
            // the source arguments; element access derefs the slot.
            if (p->isVarargRest && p->mod == ModRef)
            {
                const TypeName* elem = TypeNameArrayElem(&p->type);

                if (elem && !BuilderIsOwningType(b, elem))
                {
                    TypeDescSetAliasedArray(&sym->typeDesc, true);
                }
            }

            // An owned (non-ref) box/array parameter is consumed: freed at return.
            // A vararg rest array is stack-backed: its owning ELEMENTS are
            // dropped, but the caller's stack buffer is not freed.
            if (boxParam && p->mod == ModNone)
            {
                OwnLocal* ol = (OwnLocal*)ArenaAlloc(b->arena, sizeof(OwnLocal));
                ol->slot = sym->value;
                ol->td = typeDesc;
                ol->stackBuffer = p->isVarargRest;
                VecPush(&b->owningLocals, ol);
            }
        }
        else
        {
            LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "arg");
            LLVMBuildStore(b->builder, LLVMGetParam(b->curFn, (unsigned)(i + paramBase)), slot);

            sym->value = slot;
            sym->typeDesc = typeDesc;

            if (owningStructParam)
            {
                OwnLocal* ol = (OwnLocal*)ArenaAlloc(b->arena, sizeof(OwnLocal));
                ol->slot = slot;
                ol->td = typeDesc;
                ol->stackBuffer = false;
                VecPush(&b->owningLocals, ol);
            }
        }

        StrMapPut(&b->symbols, p->name, sym);
    }

    Block* blk = (Block*)f->body;

    PushScope(b);
    SymPush(b);

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        Node* s = (Node*)VecGet(&blk->statements, i);
        EmitStmt(b, s);

        if (b->terminated)
        {
            break;
        }
    }

    if (!b->terminated)
    {
        RunDefers(b, TopScope(b));
    }

    SymPop(b);
    PopScope(b);

    if (!b->terminated)
    {
        EmitDrops(b, 0);

        if (TypeDescIsVoid(&b->curRet))
        {
            LLVMBuildRetVoid(b->builder);
        }
        else
        {
            LLVMBuildRet(b->builder, LLVMConstNull(b->curRetAbi));
        }
    }
}

// int main(int argc, char** argv): create the context, run the Strata entry (passing on argc and argv when it
// takes them), destroy the context.
void EmitAotEntryPoint(Builder* b, const FunctionDecl* entry)
{
    FuncInfo* info = (FuncInfo*)StrMapGet(&b->funcs, entry->mangledName);
    LLVMValueRef createFn = LLVMGetNamedFunction(b->mod, ExportedSymbolName(b, "__strata_context_create"));
    LLVMValueRef destroyFn = LLVMGetNamedFunction(b->mod, ExportedSymbolName(b, "__strata_context_destroy"));

    if (!info || !info->function || !createFn || !destroyFn)
    {
        return;
    }

    LLVMTypeRef params[2] = {I32Ty(b), b->ptrTy};
    LLVMTypeRef mainTy = LLVMFunctionType(I32Ty(b), params, 2, 0);
    LLVMValueRef mainFn = LLVMAddFunction(b->mod, "main", mainTy);

    LLVMBuilderRef builder = LLVMCreateBuilderInContext(b->ctx);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(b->ctx, mainFn, "entry"));

    LLVMTypeRef createTy = LLVMFunctionType(b->ptrTy, NULL, 0, 0);
    LLVMValueRef ctx = LLVMBuildCall2(builder, createTy, createFn, NULL, 0, "ctx");

    LLVMValueRef callArgs[3] = {ctx, LLVMGetParam(mainFn, 0), LLVMGetParam(mainFn, 1)};
    LLVMValueRef result
        = LLVMBuildCall2(builder, info->type, info->function, callArgs, (unsigned)(1 + entry->params.count), "");
    LLVMValueRef exitCode
        = GetPrimitiveType(entry->returnType.name) == PrimVoid ? LLVMConstInt(I32Ty(b), 0, 0) : result;

    LLVMTypeRef destroyParams[1] = {b->ptrTy};
    LLVMTypeRef destroyTy = LLVMFunctionType(LLVMVoidTypeInContext(b->ctx), destroyParams, 1, 0);
    LLVMBuildCall2(builder, destroyTy, destroyFn, &ctx, 1, "");

    LLVMBuildRet(builder, exitCode);
    LLVMDisposeBuilder(builder);
}
