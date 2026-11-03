/* Builder: Module build entry points. */
#include "Codegen/Builder/BuilderInternal.h"

/* Setup: the builder's module, diagnostics and profile flags, and the LLVM context, module and IR builder. */
static void BuilderInitModule(Builder* b, const Module* module, DiagnosticEngine* diag, bool jitMode,
                              const StrataProfile* profile)
{
    b->m_diag = diag;
    b->m_module = module;
    b->m_jitMode = jitMode;
    b->m_boundsCheck = !profile || profile->boundsCheck;
    b->m_nullExternCheck = !profile || profile->nullExternCall;
    b->m_ctx = LLVMContextCreate();
    b->m_mod = LLVMModuleCreateWithNameInContext(module->name, b->m_ctx);
    b->m_builder = LLVMCreateBuilderInContext(b->m_ctx);
    b->m_ptrTy = LLVMPointerTypeInContext(b->m_ctx, 0);
    b->m_strLitCount = 0;
}

/* Impl property table: "Handle.Prop" -> accessors, resolved against the
   module's function list (sema guarantees the externs exist). */
static void DeclareImplProperties(Builder* b, const Module* module)
{
    for (size_t i = 0; i < module->impls.count; i++)
    {
        const ImplDecl* impl = (const ImplDecl*)VecGet((Vec*)&module->impls, i);

        for (size_t j = 0; j < impl->properties.count; j++)
        {
            PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, j);
            ImplPropEntry* entry = (ImplPropEntry*)arena_alloc(b->m_arena, sizeof(ImplPropEntry));
            entry->decl = prop;
            entry->getter = FindModuleFunction(module, prop->getterSymbol);
            entry->setter = FindModuleFunction(module, prop->setterSymbol);

            StrMapPut(&b->m_implProps, arena_format(b->m_arena, "%s.%s", impl->handleName, prop->name), entry);
        }
    }
}

/* Names every struct up front so bodies can refer to each other: opaque structs are plain pointers,
   the rest are named LLVM structs whose bodies come later. */
static void DeclareStructTypes(Builder* b)
{
    for (size_t i = 0; i < b->m_registry.count; i++)
    {
        StructType* st = &b->m_registry.types[i];

        if (st->opaque)
        {
            StrMapPut(&b->m_structTypes, st->name, (void*)b->m_ptrTy);
        }
        else
        {
            char* irName = arena_format(b->m_arena, "struct.%s", st->name);
            LLVMTypeRef ty = LLVMStructCreateNamed(b->m_ctx, irName);
            StrMapPut(&b->m_structTypes, st->name, (void*)ty);
        }
    }
}

/* The physical body of one struct. Registry-computed [n x i8] pads are interleaved before the fields they
   follow. A struct with any explicit fieldoffset is emitted packed (pad members fully determine layout);
   natural structs (padCount 0) produce the same body. */
static void DefineStructBody(Builder* b, StructType* st)
{
    size_t memberCount = st->hasLayout ? st->physicalCount : st->fields.count;
    LLVMTypeRef* members = NULL;

    if (memberCount > 0)
    {
        members = (LLVMTypeRef*)arena_alloc(b->m_arena, memberCount * sizeof(LLVMTypeRef));
    }

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
    size_t nextPad = 0;
    FieldIndex m = 0;

    if (st->isUnion && st->hasLayout)
    {
        /* One representative member (+ tail pad); every field is read and written at byte 0. */
        FieldDecl* rep = (FieldDecl*)VecGet(&st->fields, (size_t)st->unionMember);
        members[m++] = Resolve(b, &rep->type).type;

        if (st->padCount > 0)
        {
            members[m++] = LLVMArrayType(i8Ty, (unsigned)st->pads[0].bytes);
        }

        LLVMStructSetBody((LLVMTypeRef)StrMapGet(&b->m_structTypes, st->name), members, m, 0);
        return;
    }

    for (size_t j = 0; j < st->fields.count; j++)
    {
        while (st->hasLayout && nextPad < st->padCount && st->pads[nextPad].beforeField == j)
        {
            members[m++] = LLVMArrayType(i8Ty, (unsigned)st->pads[nextPad].bytes);
            nextPad++;
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, j);
        members[m++] = Resolve(b, &fieldDecl->type).type;
    }

    while (st->hasLayout && nextPad < st->padCount)
    {
        members[m++] = LLVMArrayType(i8Ty, (unsigned)st->pads[nextPad].bytes);
        nextPad++;
    }

    LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, st->name);
    LLVMStructSetBody(structTy, members, (unsigned)memberCount, st->packedLayout ? 1 : 0);
}

/* Gives every non-opaque struct its body. */
static void DefineStructBodies(Builder* b)
{
    for (size_t i = 0; i < b->m_registry.count; i++)
    {
        StructType* st = &b->m_registry.types[i];

        if (st->opaque)
        {
            continue;
        }

        DefineStructBody(b, st);
    }
}

/* Decides where one global lives: an instanced field of the context struct (appended to ctxFieldTypes), or
   a manifest constant with no storage at all. */
static void ClassifyGlobal(Builder* b, GlobalDecl* gd, LLVMTypeRef* ctxFieldTypes, size_t* ctxFieldCount)
{
    TypeDesc typeDesc = Resolve(b, &gd->type);

    /* ^T / T[] / string / alias-of-string globals always get an
       instanced field: __strata_context_create fills them at runtime
       and __strata_context_destroy drops them. */
    if (BuilderIsOwningType(b, &gd->type))
    {
        FieldIndex index = (FieldIndex)(*ctxFieldCount);
        ctxFieldTypes[(*ctxFieldCount)++] = typeDesc.type;
        StrMapPut(&b->m_globalFieldIndex, gd->name, (void*)(uintptr_t)(index + 1));

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
        sym->value = NULL; /* no static address any more; see GlobalFieldPtr */
        sym->typeDesc = typeDesc;
        StrMapPut(&b->m_globals, gd->name, sym);
        return;
    }

    /* cstring global: a `.rodata` pointer, but still an instanced field
       (a `cstring` binding is rebindable unless `const`, so per-instance
       isolation applies the same as any other mutable global). No init
       = the static empty buffer, never NULL. `cstring g2 = g;` copies
       whatever `g`'s field currently holds — both fields live in the
       same struct now, so __strata_context_create does this as a
       runtime load-then-store (in declaration order) instead of a
       compile-time constant copy. */
    bool csCopyInit = false;

    if (typeDesc.isCString && gd->init && gd->init->kind == NodeIdent)
    {
        Value* srcSym = (Value*)StrMapGet(&b->m_globals, ((IdentExpr*)gd->init)->name);

        if (srcSym && srcSym->typeDesc.isCString)
        {
            csCopyInit = true;
        }
    }

    if (typeDesc.isCString && (!gd->init || gd->init->kind == NodeStrLiteral || csCopyInit))
    {
        FieldIndex index = (FieldIndex)(*ctxFieldCount);
        ctxFieldTypes[(*ctxFieldCount)++] = typeDesc.type;
        StrMapPut(&b->m_globalFieldIndex, gd->name, (void*)(uintptr_t)(index + 1));

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
        sym->value = NULL;
        sym->typeDesc = typeDesc;
        StrMapPut(&b->m_globals, gd->name, sym);
        return;
    }

    ConstInitVal foldedVal;
    bool folded = false;

    if (gd->init)
    {
        folded = FoldConstInit(b, typeDesc, gd->init, &foldedVal);

        if (!folded && b->m_diag)
        {
            DiagErrorFmt(b->m_diag, gd->base.range,
                         "global '%s' initializer must be a compile-time constant "
                         "(scalar literals and operator expressions over them "
                         "and other const globals)",
                         gd->name);
        }
    }

    /* A `const` scalar global with a constant initializer is a MANIFEST
       constant: no storage, no symbol — uses inline the value. This is
       the C++-safe alternative to `const int X = N;` globals (C++ gives
       const globals internal linkage, so the symbol can never be linked
       against) and the enabler for `[constName]` array dimensions.
       Unaffected by instancing: no runtime state exists for it at all. */
    if (gd->type.isConst && folded)
    {
        ConstInitVal castVal = CastConstVal(b, foldedVal, typeDesc);
        LLVMValueRef constant
            = typeDesc.isFloat ? LLVMConstReal(typeDesc.type, castVal.f) : ConstIntTrunc(typeDesc.type, castVal.i);

        ConstValueSlot* cs = (ConstValueSlot*)arena_alloc(b->m_arena, sizeof(ConstValueSlot));
        cs->civ = castVal;
        cs->td = typeDesc;
        cs->constant = constant;
        StrMapPut(&b->m_constValues, gd->name, cs);
        return;
    }

    /* Mutable scalar/struct/handle/alias global (or a const one whose
       initializer failed to fold — already diagnosed above): instanced
       field, initial value stored at runtime by __strata_context_create. */
    FieldIndex index = (FieldIndex)(*ctxFieldCount);
    ctxFieldTypes[(*ctxFieldCount)++] = typeDesc.type;
    StrMapPut(&b->m_globalFieldIndex, gd->name, (void*)(uintptr_t)(index + 1));

    Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
    sym->value = NULL;
    sym->typeDesc = typeDesc;
    StrMapPut(&b->m_globals, gd->name, sym);
}

/* Module-level globals: every global that needs real runtime storage moves into ONE per-instance "context"
   struct, reached only through a hidden pointer threaded automatically through every non-extern function
   (see DeclareFunction/DefineFunction/EmitCall). A `const` scalar whose initializer folds to a compile-time
   manifest constant still gets NO storage at all. This pass must run BEFORE DefineFunction so bodies can
   resolve the globals' fields. */
static void DeclareGlobals(Builder* b, const Module* module)
{
    LLVMTypeRef* ctxFieldTypes = NULL;
    size_t ctxFieldCount = 0;

    if (module->globals.count > 0)
    {
        ctxFieldTypes = (LLVMTypeRef*)arena_alloc(b->m_arena, module->globals.count * sizeof(LLVMTypeRef));
    }

    for (size_t i = 0; i < module->globals.count; i++)
    {
        ClassifyGlobal(b, (GlobalDecl*)VecGet(&module->globals, i), ctxFieldTypes, &ctxFieldCount);
    }

    b->m_hasInstancedGlobals = ctxFieldCount > 0;

    if (b->m_hasInstancedGlobals)
    {
        b->m_globalsStructTy = LLVMStructCreateNamed(b->m_ctx, "struct.__strata_Context");
        LLVMStructSetBody(b->m_globalsStructTy, ctxFieldTypes, (unsigned)ctxFieldCount, 0);

        b->m_exportCtxGlobal = LLVMAddGlobal(b->m_mod, b->m_ptrTy, "__strata_export_ctx");
        LLVMSetInitializer(b->m_exportCtxGlobal, LLVMConstNull(b->m_ptrTy));
        LLVMSetLinkage(b->m_exportCtxGlobal, LLVMInternalLinkage);
    }
}

/* Declares every function (generic instances and delegates too) so that any body can call any of them. */
static void DeclareFunctions(Builder* b, const Module* module)
{
    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);
        DeclareFunction(b, f);
    }

    /* After the generics themselves, so each instantiation finds its generic's JIT slot. */
    for (size_t i = 0; i < module->genericInstances.count; i++)
    {
        DeclareFunction(b, (FunctionDecl*)VecGet(&module->genericInstances, i));
    }

    for (size_t i = 0; i < module->delegates.count; i++)
    {
        DeclareFunction(b, (FunctionDecl*)VecGet(&module->delegates, i));
    }
}

/* Emits the body of every declared function. */
static void DefineFunctionBodies(Builder* b, const Module* module)
{
    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);
        DefineFunction(b, f);
    }
}

/* Starts a body for one of the context functions: a fresh symbol table, temporaries and loop stack, and an
   entry block positioned for emission. */
static void BeginContextFunction(Builder* b, LLVMValueRef fn, TypeDesc ret, LLVMTypeRef retAbi)
{
    b->m_curFn = fn;
    StrMapClear(&b->m_symbols);
    b->m_terminated = false;
    b->m_loops.count = 0;
    b->m_owningLocals.count = 0;
    ResetTemps(b);
    b->m_curRet = ret;
    b->m_curRetAbi = retAbi;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "entry");
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;
    LLVMPositionBuilderAtEnd(b->m_builder, entry);
}

/* Context functions when the module has no instanced globals: create returns NULL and destroy does nothing. */
static void EmitContextStubs(Builder* b)
{
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(b->m_ctx);

    LLVMTypeRef createTy = LLVMFunctionType(b->m_ptrTy, NULL, 0, 0);
    LLVMValueRef createFn = LLVMAddFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_create"), createTy);
    LLVMPositionBuilderAtEnd(b->m_builder, LLVMAppendBasicBlockInContext(b->m_ctx, createFn, "entry"));
    LLVMBuildRet(b->m_builder, LLVMConstNull(b->m_ptrTy));

    LLVMTypeRef destroyParams[1] = {b->m_ptrTy};
    LLVMTypeRef destroyTy = LLVMFunctionType(voidTy, destroyParams, 1, 0);
    LLVMValueRef destroyFn = LLVMAddFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_destroy"), destroyTy);
    LLVMPositionBuilderAtEnd(b->m_builder, LLVMAppendBasicBlockInContext(b->m_ctx, destroyFn, "entry"));
    LLVMBuildRetVoid(b->m_builder);
}

/* An owning global (^T, T[], string): the initializer is constructed into the field. */
static void InitOwningContextGlobal(Builder* b, GlobalDecl* gd, TypeDesc td, LLVMValueRef fieldPtr)
{
    if (!gd->init)
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(td.type), fieldPtr);
    }
    else if (TypeDescHasArrayShape(&td) && gd->init->kind == NodeArrayInit)
    {
        LLVMValueRef arr = EmitArrayInit(b, AsNode(ArrayInitExpr, gd->init)).value;
        LLVMBuildStore(b->m_builder, arr, fieldPtr);
    }
    else if (td.isString)
    {
        /* string / alias-of-string global: construct the owned
           value directly into the field (heap-copy a literal;
           take a call result). Teardown drops it. */
        Value val = EmitExpr(b, gd->init);
        LLVMValueRef owned = EmitOwnedValue(b, val, gd->init, StringTypeName(b));
        LLVMBuildStore(b->m_builder, owned, fieldPtr);
    }
    else if (TypeDescIsBox(&td) && td.boxInner)
    {
        Value val = EmitExpr(b, gd->init);

        /* Direct move from the same ^T kind: take pointer. */
        bool sameBoxKind = val.typeDesc.boxInner && strcmp(val.typeDesc.boxInner->name, td.boxInner->name) == 0;

        if (sameBoxKind)
        {
            LLVMBuildStore(b->m_builder, val.value, fieldPtr);
        }
        else
        {
            /* Box up the inner value (owning or not). */
            LLVMValueRef heap = EmitBoxCell(b, td.boxInner, val, gd->init, "boxgl");
            LLVMBuildStore(b->m_builder, heap, fieldPtr);
        }
    }
    else
    {
        /* Any other owning initializer (an array or owning struct
           from a call or literal): sema has already rejected moves
           from variables, so this is a fresh value we take. Without
           this the field would be left as uninitialized heap. */
        Value val = EmitExpr(b, gd->init);
        LLVMValueRef owned = EmitOwnedValue(b, val, gd->init, &gd->type);
        LLVMBuildStore(b->m_builder, owned, fieldPtr);
    }
}

/* A cstring global: a `.rodata` pointer, or a copy of another cstring global's field. */
static void InitCStringContextGlobal(Builder* b, GlobalDecl* gd, TypeDesc td, LLVMValueRef fieldPtr)
{
    bool csCopyInit = false;

    if (gd->init && gd->init->kind == NodeIdent)
    {
        Value* srcSym = (Value*)StrMapGet(&b->m_globals, ((IdentExpr*)gd->init)->name);
        csCopyInit
            = srcSym && srcSym->typeDesc.isCString && StrMapGet(&b->m_globalFieldIndex, ((IdentExpr*)gd->init)->name);
    }

    LLVMValueRef value;

    if (!gd->init)
    {
        value = EmptyNulString(b);
    }
    else if (csCopyInit)
    {
        /* Runtime load: the source field, declared earlier, is
           already initialized by this same forward pass. */
        LLVMValueRef srcPtr = GlobalFieldPtr(b, ((IdentExpr*)gd->init)->name);
        value = LLVMBuildLoad2(b->m_builder, td.type, srcPtr, "cscopy");
    }
    else if (gd->init->kind == NodeStrLiteral)
    {
        value = EmitCStringLiteralPtr(b, (StrLiteral*)gd->init);
    }
    else
    {
        /* Anything else already failed FoldConstInit above (pass
           1) and was diagnosed; store a harmless empty value. */
        value = EmptyNulString(b);
    }

    LLVMBuildStore(b->m_builder, value, fieldPtr);
}

/* A mutable scalar, struct, handle or alias field: re-derive the same compile-time fold pass 1 already
   validated (and diagnosed on failure), store zero on failure or absence. */
static void InitScalarContextGlobal(Builder* b, GlobalDecl* gd, TypeDesc td, LLVMValueRef fieldPtr)
{
    ConstInitVal foldedVal;
    bool folded = gd->init && FoldConstInit(b, td, gd->init, &foldedVal);
    LLVMValueRef value = DefaultValueOf(b, &gd->type, td);

    if (folded)
    {
        foldedVal = CastConstVal(b, foldedVal, td);
        value = td.isFloat ? LLVMConstReal(td.type, foldedVal.f) : ConstIntTrunc(td.type, foldedVal.i);
    }

    LLVMBuildStore(b->m_builder, value, fieldPtr);
}

/* Stores one global's initial value into its context field. Manifest constants have no field. */
static void InitContextGlobal(Builder* b, GlobalDecl* gd)
{
    if (!StrMapGet(&b->m_globalFieldIndex, gd->name))
    {
        return; /* manifest constant: no field, nothing to store */
    }

    TypeDesc td = Resolve(b, &gd->type);
    LLVMValueRef fieldPtr = GlobalFieldPtr(b, gd->name);

    if (BuilderIsOwningType(b, &gd->type))
    {
        InitOwningContextGlobal(b, gd, td, fieldPtr);
    }
    else if (td.isCString)
    {
        InitCStringContextGlobal(b, gd, td, fieldPtr);
    }
    else
    {
        InitScalarContextGlobal(b, gd, td, fieldPtr);
    }
}

/* __strata_context_create: allocates the context and initializes every global in declaration order. */
static void EmitContextCreate(Builder* b, const Module* module)
{
    LLVMTypeRef createTy = LLVMFunctionType(b->m_ptrTy, NULL, 0, 0);
    LLVMValueRef createFn = LLVMAddFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_create"), createTy);
    BeginContextFunction(b, createFn, TypeDescMake(b->m_ptrTy, 0, NULL), b->m_ptrTy);

    /* strata_alloc does NOT zero memory: every field must be explicitly
       stored below (unlike the old static-initializer globals, which
       zero-filled for free). Set as this function's own hidden context
       pointer BEFORE evaluating any initializer, since an owning
       global's initializer may legally be a call to another non-extern
       Strata function (sema allows "... or a call returning T"), and
       that call needs a context to forward. */
    LLVMValueRef allocArgs[1] = {SizeOfConst(b, b->m_globalsStructTy)};
    StrataAllocFn(b);
    LLVMValueRef ctxMem = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "ctxmem");
    b->m_curGlobalsPtr = ctxMem;

    /* The newest context is the one `@export` bodies (C callbacks) run against. */
    LLVMBuildStore(b->m_builder, ctxMem, b->m_exportCtxGlobal);

    for (size_t i = 0; i < module->globals.count; i++)
    {
        InitContextGlobal(b, (GlobalDecl*)VecGet(&module->globals, i));
    }

    /* Temporaries borrowed by global initializers. */
    FlushTemps(b, 0);

    LLVMBuildRet(b->m_builder, ctxMem);
}

/* Drops one global's field at context destruction, if the global owns heap data. */
static void DropContextGlobal(Builder* b, GlobalDecl* gd)
{
    if (!BuilderIsOwningType(b, &gd->type) && !BuilderIsOwningValue(b, &gd->type))
    {
        return;
    }

    if (!StrMapGet(&b->m_globalFieldIndex, gd->name))
    {
        return;
    }

    TypeDesc td = Resolve(b, &gd->type);
    EmitDropOne(b, GlobalFieldPtr(b, gd->name), td);
}

/* __strata_context_destroy: drops the globals in reverse declaration order, unbinds the @export context, and
   frees the context. */
static void EmitContextDestroy(Builder* b, const Module* module)
{
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(b->m_ctx);
    LLVMTypeRef destroyParams[1] = {b->m_ptrTy};
    LLVMTypeRef destroyTy = LLVMFunctionType(voidTy, destroyParams, 1, 0);
    LLVMValueRef destroyFn = LLVMAddFunction(b->m_mod, ExportedSymbolName(b, "__strata_context_destroy"), destroyTy);
    BeginContextFunction(b, destroyFn, TypeDescMake(voidTy, TDF_VOID, NULL), voidTy);

    b->m_curGlobalsPtr = LLVMGetParam(destroyFn, 0);

    /* Globals are built in declaration order and destroyed in the reverse, so a later global's Drop can still
       read an earlier one. A struct that owns heap data through its fields is destroyed too. */
    for (size_t i = module->globals.count; i-- > 0;)
    {
        DropContextGlobal(b, (GlobalDecl*)VecGet(&module->globals, i));
    }

    /* Destroying the `@export` context unbinds it. */
    LLVMValueRef boundCtx = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, b->m_exportCtxGlobal, "boundctx");
    LLVMValueRef isBound = LLVMBuildICmp(b->m_builder, LLVMIntEQ, boundCtx, b->m_curGlobalsPtr, "isbound");
    LLVMBuildStore(b->m_builder, LLVMBuildSelect(b->m_builder, isBound, LLVMConstNull(b->m_ptrTy), boundCtx, "unbind"),
                   b->m_exportCtxGlobal);

    LLVMValueRef freeArgs[1] = {b->m_curGlobalsPtr};
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

    LLVMBuildRetVoid(b->m_builder);
}

/* Every module exports __strata_context_create + __strata_context_destroy. Unlike the old per-process
   __strata_module_init/teardown, these are NEVER auto-registered (no llvm.global_ctors) in either mode: the
   whole point of instancing is that a host may create many independent contexts, so it always calls these
   explicitly — discoverable/callable exactly like any other exported Strata function (JIT:
   strataJitGetFunction; AOT: a hand-written extern prototype). */
static void EmitContextFunctions(Builder* b, const Module* module)
{
    if (!b->m_hasInstancedGlobals)
    {
        EmitContextStubs(b);
        return;
    }

    EmitContextCreate(b, module);
    EmitContextDestroy(b, module);
}

/* Emits the @export entry points the module declares. */
static void EmitAotEntryPoints(Builder* b, const Module* module)
{
    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);

        if (IsAotEntryPoint(b, f))
        {
            EmitAotEntryPoint(b, f);
        }
    }
}

/* Records the module's extern declarations so the host can bind them by name. */
static void CollectExternNames(Builder* b, const Module* module)
{
    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);

        if (f->isExtern)
        {
            VecPush(&b->m_externNames, f->name);
        }
    }
}

/* The same blob strataCompileTypeMetadata produces, as a read-only symbol the host can find in an AOT object
   (the JIT hands out the copy kept on the BuiltModule). */
static void EmitTypeMetadataGlobal(Builder* b, const Module* module, uint8_t** typeMetadata, size_t* typeMetadataSize)
{
    if (TypeMetadataBuild(&b->m_registry, module, typeMetadata, typeMetadataSize))
    {
        LLVMValueRef bytes
            = LLVMConstStringInContext(b->m_ctx, (const char*)*typeMetadata, (unsigned)*typeMetadataSize, 1);
        LLVMValueRef global = LLVMAddGlobal(b->m_mod, LLVMTypeOf(bytes), ExportedSymbolName(b, STRATA_TYPES_SYMBOL));
        LLVMSetInitializer(global, bytes);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetAlignment(global, 16);
    }
}

/* Finalizes the module: disposes the IR builder, emits the type metadata, and hands the LLVM context and
   module over in the result. */
static BuiltModule BuilderFinalize(Builder* b, const Module* module)
{
    if (b->m_builder)
    {
        LLVMDisposeBuilder(b->m_builder);
        b->m_builder = NULL;
    }

    uint8_t* typeMetadata = NULL;
    size_t typeMetadataSize = 0;

    EmitTypeMetadataGlobal(b, module, &typeMetadata, &typeMetadataSize);

    BuiltModule out;
    BuiltModuleInit(&out);

    out.ctx = b->m_ctx;
    out.mod = b->m_mod;
    out.externSymbols = b->m_externNames;
    out.hasInstancedGlobals = b->m_hasInstancedGlobals;
    out.typeMetadata = typeMetadata;
    out.typeMetadataSize = typeMetadataSize;

    b->m_ctx = NULL;
    b->m_mod = NULL;

    return out;
}

static BuiltModule BuilderBuild(Builder* b, const Module* module, DiagnosticEngine* diag, bool jitMode,
                                const StrataProfile* profile)
{
    BuilderInitModule(b, module, diag, jitMode, profile);

    /* Declare types: impl property table, struct names, then struct bodies. */
    TypeRegistryBuild(&b->m_registry, module);
    DeclareImplProperties(b, module);
    DeclareStructTypes(b);
    DefineStructBodies(b);

    /* Globals are classified before any function is declared, because the context struct they fill is what
       bodies resolve against. */
    DeclareGlobals(b, module);

    DeclareFunctions(b, module);
    DefineFunctionBodies(b, module);

    /* Globals' bodies: the context create/destroy functions. */
    EmitContextFunctions(b, module);

    EmitAotEntryPoints(b, module);
    CollectExternNames(b, module);

    return BuilderFinalize(b, module);
}

void BuiltModuleInit(BuiltModule* bm)
{
    bm->ctx = NULL;
    bm->mod = NULL;
    bm->hasInstancedGlobals = false;
    bm->typeMetadata = NULL;
    bm->typeMetadataSize = 0;
    VecInit(&bm->externSymbols);
}

void BuiltModuleDispose(BuiltModule* bm)
{
    if (bm->mod)
    {
        LLVMDisposeModule(bm->mod);
        bm->mod = NULL;
    }
    if (bm->ctx)
    {
        LLVMContextDispose(bm->ctx);
        bm->ctx = NULL;
    }
    if (bm->externSymbols.items)
    {
        free(bm->externSymbols.items);
        bm->externSymbols.items = NULL;
        bm->externSymbols.count = 0;
        bm->externSymbols.cap = 0;
    }

    free(bm->typeMetadata);
    bm->typeMetadata = NULL;
    bm->typeMetadataSize = 0;
}

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                            const StrataProfile* profile)
{
    return BuildLlvmModuleEx(ast, diag, arena, jitMode, profile, NULL);
}

BuiltModule BuildLlvmModuleEx(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                              const StrataProfile* profile, const char* symbolPrefix)
{
    Builder b = {0};
    b.m_arena = arena;
    b.m_symbolPrefix = symbolPrefix;
    StrMapInit(&b.m_structDefaults);
    StrMapInit(&b.m_typeDescs);
    StrMapInit(&b.m_structTypes);
    StrMapInit(&b.m_funcs);
    StrMapInit(&b.m_symbols);
    StrMapInit(&b.m_globals);
    StrMapInit(&b.m_globalFieldIndex);
    StrMapInit(&b.m_constValues);
    StrMapInit(&b.m_externSlots);
    StrMapInit(&b.m_implProps);
    StrMapInit(&b.m_dropFns);
    StrMapInit(&b.m_copyFns);
    StrMapInit(&b.m_eqHelpers);
    VecInit(&b.m_externNames);
    VecInit(&b.m_loops);
    VecInit(&b.m_owningLocals);
    VecInit(&b.m_temps);
    VecInit(&b.m_freshOwned);
    VecInit(&b.m_tempParts);
    VecInit(&b.m_scopes);
    VecInit(&b.m_symDecls);
    b.m_allocFn = NULL;
    b.m_allocFnType = NULL;
    b.m_freeFn = NULL;
    b.m_freeFnType = NULL;
    b.m_panicFn = NULL;
    b.m_panicFnType = NULL;
    b.m_oobFn = NULL;
    b.m_oobFnType = NULL;
    b.m_strdupFn = NULL;
    b.m_strdupFnType = NULL;
    b.m_strEqFn = NULL;
    b.m_strEqFnType = NULL;
    b.m_csLenFn = NULL;
    b.m_csLenFnType = NULL;
    b.m_arrayType = NULL;

    TypeRegistryInit(&b.m_registry);

    BuiltModule module = BuilderBuild(&b, ast, diag, jitMode, profile);

    if (module.mod)
    {
        MarkPackedFieldAccessesUnaligned(module.mod);
    }
    StrMapFree(&b.m_structTypes);
    StrMapFree(&b.m_funcs);
    StrMapFree(&b.m_symbols);
    StrMapFree(&b.m_globals);
    StrMapFree(&b.m_globalFieldIndex);
    StrMapFree(&b.m_constValues);
    StrMapFree(&b.m_externSlots);
    StrMapFree(&b.m_implProps);
    StrMapFree(&b.m_dropFns);
    StrMapFree(&b.m_copyFns);
    StrMapFree(&b.m_eqHelpers);
    StrMapFree(&b.m_structDefaults);
    StrMapFree(&b.m_typeDescs);
    free(b.m_loops.items);
    free(b.m_owningLocals.items);
    free(b.m_temps.items);
    free(b.m_freshOwned.items);
    free(b.m_tempParts.items);
    TypeRegistryFree(&b.m_registry);

    return module;
}
