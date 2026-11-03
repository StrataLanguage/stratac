/* Declaration-level checks and the ResolveOverloads entry point. */
#include "Sema/Resolve/ResolverInternal.h"

/* Why a component field of `type` isn't plain data (NULL when it is). The offending
   type's spelling goes to `outOffender`. Nested structs and fixed-array elements are checked
   too, since the host copies components as raw bytes. */
static const char* ComponentFieldProblem(Resolver* r, const TypeName* type, const char** outOffender, size_t depth)
{
    if (!type || !type->name || depth > r->m_registry.count + 8)
    {
        return NULL;
    }

    *outOffender = type->name;

    if (TypeNameIsBox(type))
    {
        return "an owning box";
    }

    if (TypeNameIsOptional(type))
    {
        return "an optional";
    }

    if (TypeNameHasArrayShape(type))
    {
        if (type->length < 0)
        {
            return "a dynamic array";
        }

        return ComponentFieldProblem(r, type->elem, outOffender, depth + 1);
    }

    if (type->primitiveType == PrimString)
    {
        return "a string";
    }

    if (type->primitiveType == PrimCString)
    {
        return "a cstring";
    }

    const StructType* registered = TypeRegistryFind(&r->m_registry, type->name);

    if (!registered || registered->opaque)
    {
        return NULL;
    }

    if (registered->isTypeAlias)
    {
        if (!registered->underlyingType)
        {
            return NULL;
        }

        TypeName underlying = TypeNameParse(r->m_arena, registered->underlyingType);

        return ComponentFieldProblem(r, &underlying, outOffender, depth + 1);
    }

    for (size_t i = 0; i < registered->fields.count; i++)
    {
        const FieldDecl* nested = (const FieldDecl*)VecGet(&registered->fields, i);
        const char* problem = ComponentFieldProblem(r, &nested->type, outOffender, depth + 1);

        if (problem)
        {
            return problem;
        }
    }

    return NULL;
}

static void CheckComponentFields(Resolver* r, const StructDecl* sd)
{
    for (size_t i = 0; i < sd->fields.count; i++)
    {
        const FieldDecl* field = (const FieldDecl*)VecGet(&sd->fields, i);
        const char* offender = NULL;
        const char* problem = ComponentFieldProblem(r, &field->type, &offender, 0);

        if (!problem)
        {
            continue;
        }

        if (offender && strcmp(offender, field->type.name) != 0)
        {
            DiagErrorFmt(r->m_diag, field->type.range,
                         "component '%s' field '%s' has type '%s', which contains '%s' (%s); components may only "
                         "hold plain data: scalars, enums, float2/3/4, handles, fixed-size arrays and structs of those",
                         sd->name, field->name, field->type.name, offender, problem);
        }
        else
        {
            DiagErrorFmt(r->m_diag, field->type.range,
                         "component '%s' field '%s' has type '%s', which is %s; components may only hold plain data: "
                         "scalars, enums, float2/3/4, handles, fixed-size arrays and structs of those",
                         sd->name, field->name, field->type.name, problem);
        }
    }
}

/* Collects the lanes of a constant float2/3/4 constructor (nested constructors flatten). */
static bool FoldVectorDefaultLanes(Resolver* r, Node* n, double* lanes, unsigned* laneCount, unsigned maxLanes)
{
    if (n && n->kind == NodeCall)
    {
        CallExpr* call = (CallExpr*)n;

        if (!call->isIntrinsicCall || IsNameSimdVector(call->callee) == 0)
        {
            return false;
        }

        for (size_t i = 0; i < call->args.count; i++)
        {
            if (!FoldVectorDefaultLanes(r, (Node*)VecGet(&call->args, i), lanes, laneCount, maxLanes))
            {
                return false;
            }
        }

        return true;
    }

    ConstGlobalVal value = {0};

    if (*laneCount >= maxLanes || !SemaFoldConstInit(r, n, &value))
    {
        return false;
    }

    lanes[(*laneCount)++] = value.isFloat ? value.f : (double)value.i;

    return true;
}

/* Type-checks a field's `= expr` default and folds it into `field->folded`. */
static void ResolveFieldDefault(Resolver* r, FieldDecl* field, StrMap* scope)
{
    const TypeName* type = &field->type;
    const char* leaf = (TypeNameHasArrayShape(type) || TypeNameIsBox(type) || TypeNameIsOptional(type))
                           ? NULL
                           : TypeRegistryResolveAlias(&r->m_registry, type->name);
    PrimitiveType primitive = leaf ? GetPrimitiveType(leaf) : PrimNone;
    unsigned vectorLanes = leaf ? (unsigned)IsNameSimdVector(leaf) : 0;

    if (!leaf || (!IsNumeric(primitive) && vectorLanes == 0))
    {
        DiagErrorFmt(r->m_diag, field->defaultValue->range,
                     "field '%s' of type '%s' can't have a default value; defaults are only supported on scalar, "
                     "enum and float2/3/4 fields",
                     field->name, type->name);
        return;
    }

    ResolveExpr(r, field->defaultValue, scope);
    const TypeName* valueType = InferType(r, field->defaultValue, scope);

    if (valueType && !IsAssignableType(r, type, valueType))
    {
        DiagErrorFmt(r->m_diag, field->defaultValue->range,
                     "default value for field '%s' of type '%s' can't be of type '%s'", field->name, type->name,
                     valueType->name);
        return;
    }

    FieldDefaultValue folded = {0};
    folded.isFloat = vectorLanes > 0 || primitive == PrimFloat || primitive == PrimDouble;

    if (vectorLanes > 0)
    {
        if (!FoldVectorDefaultLanes(r, field->defaultValue, folded.floatLanes, &folded.laneCount, vectorLanes)
            || folded.laneCount != vectorLanes)
        {
            DiagErrorFmt(r->m_diag, field->defaultValue->range,
                         "default value for field '%s' must be a compile-time constant '%s(...)'", field->name, leaf);
            return;
        }
    }
    else
    {
        ConstGlobalVal value = {0};
        value.isInt = !folded.isFloat;

        if (!SemaFoldConstInit(r, field->defaultValue, &value))
        {
            DiagErrorFmt(r->m_diag, field->defaultValue->range,
                         "default value for field '%s' must be a compile-time constant", field->name);
            return;
        }

        folded.laneCount = 1;

        if (folded.isFloat)
        {
            folded.floatLanes[0] = value.isFloat ? value.f : (double)value.i;
        }
        else if (primitive == PrimBool)
        {
            folded.intValue = (value.isFloat ? value.f != 0.0 : value.i != 0) ? 1 : 0;
        }
        else
        {
            folded.intValue = value.isFloat ? (ConstInt)value.f : value.i;
        }
    }

    folded.isSet = true;
    field->folded = folded;
}

/* `void Drop(ref T self)` in a struct's impl is T's destructor. Binds it to the struct and gives the
   struct its hidden `__alive` byte (default 1): moves zero the source, so a moved-from value reads 0
   and is not dropped twice. Runs before the type registry is built, so the field is in every layout. */
static void BindDropMethods(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    for (size_t i = 0; i < mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, i);
        StructDecl* sd = NULL;

        for (size_t k = 0; k < mod->structs.count; k++)
        {
            StructDecl* candidate = (StructDecl*)VecGet(&mod->structs, k);

            if (!candidate->incomplete && !candidate->isTypeAlias && strcmp(candidate->name, impl->handleName) == 0)
            {
                sd = candidate;
                break;
            }
        }

        for (size_t j = 0; j < impl->methods.count; j++)
        {
            FunctionDecl* method = (FunctionDecl*)VecGet(&impl->methods, j);

            if (!method->methodName || strcmp(method->methodName, "Drop") != 0)
            {
                continue;
            }

            if (!sd)
            {
                DiagErrorFmt(diag, method->base.range, "'Drop' is only allowed in the impl of a struct with a body");
                continue;
            }

            const ParamDecl* self = method->params.count == 1 ? (const ParamDecl*)VecGet(&method->params, 0) : NULL;
            bool shape = self && self->mod == ModRef && strcmp(self->type.name, sd->name) == 0
                         && GetPrimitiveType(method->returnType.name) == PrimVoid && method->body;

            if (!shape)
            {
                DiagErrorFmt(diag, method->base.range, "a destructor must be declared 'void Drop(ref %s self) { ... }'",
                             sd->name);
                continue;
            }

            if (sd->isExtern || sd->isUnion || sd->isComponent)
            {
                DiagErrorFmt(diag, method->base.range,
                             "'%s' cannot have a Drop method: extern structs, unions and components are plain data",
                             sd->name);
                continue;
            }

            if (sd->dropFunction)
            {
                DiagErrorFmt(diag, method->base.range, "'%s' already has a Drop method", sd->name);
                continue;
            }

            sd->dropFunction = method;

            FieldDecl* alive = AST_NEW(arena, FieldDecl);
            alive->type = TypeNameLeaf((char*)"byte");
            alive->name = (char*)STRATA_ALIVE_FIELD;
            alive->offset = -1;
            alive->nameRange = sd->base.range;
            alive->folded.isSet = true;
            alive->folded.laneCount = 1;
            alive->folded.intValue = 1;
            VecPush(&sd->fields, alive);
        }
    }
}

/* Same C prototype: return type, parameter types/modifiers, variadic-ness. */
static bool SameExternSignature(const FunctionDecl* a, const FunctionDecl* b)
{
    if (strcmp(a->returnType.name, b->returnType.name) != 0 || a->params.count != b->params.count
        || a->isCVararg != b->isCVararg || a->isVariadic != b->isVariadic || !a->typeParam != !b->typeParam)
    {
        return false;
    }

    for (size_t i = 0; i < a->params.count; i++)
    {
        const ParamDecl* pa = (const ParamDecl*)VecGet((Vec*)&a->params, i);
        const ParamDecl* pb = (const ParamDecl*)VecGet((Vec*)&b->params, i);

        if (pa->mod != pb->mod || pa->isReturn != pb->isReturn || pa->type.isConst != pb->type.isConst
            || strcmp(pa->type.name, pb->type.name) != 0)
        {
            return false;
        }
    }

    return true;
}

/* An extern may be declared more than once (two binding files both declaring `printf`), as in C:
   identical redeclarations collapse into the first; a conflicting one is an error. */
static void MergeExternRedeclarations(Module* mod, DiagnosticEngine* diag)
{
    size_t kept = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);
        bool duplicate = false;

        for (size_t j = 0; f->isExtern && !f->fromImpl && j < kept; j++)
        {
            const FunctionDecl* earlier = (const FunctionDecl*)VecGet(&mod->functions, j);

            if (!earlier->isExtern || earlier->fromImpl || strcmp(earlier->name, f->name) != 0)
            {
                continue;
            }

            if (SameExternSignature(earlier, f))
            {
                duplicate = true;
            }
            else
            {
                DiagErrorFmt(diag, f->base.range, "conflicting declarations of extern function '%s'", f->name);
                duplicate = true;
            }

            break;
        }

        if (!duplicate)
        {
            VecSet(&mod->functions, kept++, f);
        }
    }

    mod->functions.count = kept;
}

/* `Name(args)` constructs the struct `Name` when there is one, so a function of the same name could
   never be called: its calls would silently build the struct from their arguments instead. */
static void RejectFunctionsNamedLikeStructs(Module* mod, DiagnosticEngine* diag)
{
    StrMap structs;
    StrMapInit(&structs);

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (!sd->isTypeAlias)
        {
            StrMapPut(&structs, sd->name, sd);
        }
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* f = (const FunctionDecl*)VecGet(&mod->functions, i);

        if (!f->fromImpl && f->name && StrMapGet(&structs, f->name))
        {
            DiagErrorFmt(diag, f->base.range,
                         "function '%s' has the name of a struct: 'Name(...)' constructs the struct, so the function "
                         "could never be called; rename one of them",
                         f->name);
        }
    }

    StrMapFree(&structs);
}

/* Pass 1: merge extern redeclarations and mark the declarations the later passes read. */
static void CollectDeclarations(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;
    Arena* arena = r->m_arena;

    MergeExternRedeclarations(mod, diag);
    MarkGrowableArrayParams(mod);
    BindDropMethods(mod, diag, arena);

    TypeRegistryInit(&r->m_registry);
}

static void RegisterTypeAliases(Resolver* r)
{
    Module* mod = r->m_mod;

    TypeRegistryRegisterAliases(&r->m_registry, mod);
    StrMapInit(&r->m_constGlobals);
}

static void ResolveEnumDecls(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    /* Assign enum member values and validate underlying ranges, BEFORE the
       manifest-constant fold so `const int N = (int)Color.Red;` resolves. */
    ResolveEnums(mod, diag, &r->m_registry);
}

static void FoldManifestConstants(Resolver* r)
{
    Module* mod = r->m_mod;
    Arena* arena = r->m_arena;

    /* Manifest constants: fold `const` scalar global initializers FIRST, so
       `[constName]` fixed-array dimensions resolve before layouts compute.
       Aliases register first — a `const AliasType` folds through to its
       underlying scalar. */

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (!gd->type.isConst || !gd->init || !IsManifestConstType(&r->m_registry, gd->type.name))
        {
            continue;
        }

        ConstGlobalVal* v = (ConstGlobalVal*)ArenaAlloc(arena, sizeof(ConstGlobalVal));
        v->isInt = IsConstDimType(&r->m_registry, gd->type.name);

        if (SemaFoldConstInit(r, gd->init, v))
        {
            /* The constant takes its declared type: later uses read it at that width and signedness (a
               `const uint` initialized with -1 holds 0xFFFFFFFF). */
            v->ity = SemaConstIntType(GetPrimitiveType(TypeRegistryResolveAlias(&r->m_registry, gd->type.name)));

            if (!v->isFloat)
            {
                v->i = (ConstInt)ConstIntNarrow((ConstUInt)v->i, v->ity);
            }

            StrMapPut(&r->m_constGlobals, gd->name, v);
        }
    }
}

static void ResolveStructDims(Resolver* r)
{
    Module* mod = r->m_mod;

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (sd->incomplete || sd->isTypeAlias)
        {
            continue;
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            SemaResolveConstDims(r, &field->type);
        }
    }
}

static void BuildRegistryAndImpls(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;
    Arena* arena = r->m_arena;

    TypeRegistryBuild(&r->m_registry, mod);
    RejectFunctionsNamedLikeStructs(mod, diag);

    /* Validate impl targets and declare property accessor externs before the
       overload/mangling pass runs. */
    ResolveImpls(mod, diag, arena, &r->m_registry);
}

static void CheckUnionFields(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    /* A union's fields overlay one another, so none may own memory (nothing could tell which to drop). */
    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        for (size_t j = 0; sd->isUnion && j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);
            const char* leaf = TypeRegistryResolveAlias(&r->m_registry, field->type.name);

            if (AliasIsOwning(r, &field->type) || TypeRegistryIsOwningStruct(&r->m_registry, leaf))
            {
                DiagErrorFmt(diag, field->nameRange, "union field '%s' cannot have owning type '%s'", field->name,
                             field->type.name);
            }
        }
    }
}

static void InitResolverState(Resolver* r)
{
    Module* mod = r->m_mod;

    StrMapInit(&r->m_constVars);
    StrMapInit(&r->m_boxGlobals);
    StrMapInit(&r->m_refBoxParams);
    StrMapInit(&r->m_refLocals);
    StrMapInit(&r->m_refArrayParams);
    StrMapInit(&r->m_typeCache);
    VecInit(&r->m_scopeDecls);
    VecInit(&r->m_deferFrames);
    VecInit(&r->m_loopFlows);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (AliasIsOwning(r, &gd->type))
        {
            StrMapPut(&r->m_boxGlobals, gd->name, (void*)&gd->type);
        }
    }
}

static void MangleFunctions(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;
    Arena* arena = r->m_arena;

    StrMap byMangled;
    StrMapInit(&byMangled);

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        bool overloaded = CountByName(mod, functionDecl->name) > 1;

        if (overloaded && (functionDecl->isExtern || functionDecl->isExport))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "%s function '%s' cannot be overloaded",
                         functionDecl->isExtern ? "extern" : "'@export'", functionDecl->name);
        }

        if (functionDecl->isExport)
        {
            CheckExportDecl(r, functionDecl);
        }

        functionDecl->mangledName = overloaded ? Mangle(arena, functionDecl) : functionDecl->name;

        if (StrMapGet(&byMangled, functionDecl->mangledName))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "duplicate function signature for '%s'", functionDecl->name);
        }

        StrMapPut(&byMangled, functionDecl->mangledName, functionDecl);
    }

    StrMapFree(&byMangled);
}

static void CheckDeclaredFunctions(Resolver* r)
{
    Module* mod = r->m_mod;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (functionDecl->typeParam)
        {
            CheckGenericExternDecl(r, functionDecl);
        }
    }

    for (size_t i = 0; i < mod->delegates.count; i++)
    {
        CheckDelegateDecl(r, (FunctionDecl*)VecGet(&mod->delegates, i));
    }

    InferBorrowedParams(r);
}

static void CheckReturnPaths(DiagnosticEngine* diag, const FunctionDecl* functionDecl)
{
    if (functionDecl->isNoReturn)
    {
        if (functionDecl->returnType.primitiveType != PrimVoid)
        {
            DiagErrorFmt(diag, functionDecl->base.range, "@noreturn function '%s' must return 'void'",
                         functionDecl->name);
        }
        else if (functionDecl->hasReturnStmt)
        {
            DiagErrorFmt(diag, functionDecl->base.range, "@noreturn function '%s' contains a return statement",
                         functionDecl->name);
        }
        else if (StmtFallsThrough(functionDecl->body, 0))
        {
            DiagErrorFmt(diag, functionDecl->base.range,
                         "@noreturn function '%s' can reach the end of its body; end every path in a call to "
                         "another @noreturn function (exit, abort, ...)",
                         functionDecl->name);
        }
    }

    /* A non-void function must return on every path; if control can fall
       off the end of the body, at least one path is missing a return. */
    if (functionDecl->returnType.primitiveType != PrimVoid && StmtFallsThrough(functionDecl->body, 0))
    {
        DiagErrorFmt(diag, functionDecl->base.range, "missing return statement in function '%s' returning '%s'",
                     functionDecl->name, functionDecl->returnType.name);
    }
}

static void ResolveFunctionBody(Resolver* r, FunctionDecl* functionDecl)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    StrMap scope;
    StrMapInit(&scope);

    for (size_t j = 0; j < mod->globals.count; j++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, j);
        StrMapPut(&scope, gd->name, (void*)&gd->type);
    }

    // All flow state is per function: nothing proven in one holds in the next.
    ResetStrMap(&r->m_constVars);
    ResetStrMap(&r->m_refBoxParams);
    ResetStrMap(&r->m_refLocals);
    ResetStrMap(&r->m_refArrayParams);
    r->m_scopeDecls.count = 0;
    r->m_deferFrames.count = 0;
    r->m_loopFlows.count = 0;
    r->m_deferWalkDepth = 0;
    r->m_unreachable = false;

    for (size_t j = 0; j < mod->globals.count; j++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, j);

        if (gd->type.isConst)
        {
            StrMapPut(&r->m_constVars, gd->name, (void*)1);
        }
    }

    for (size_t j = 0; j < functionDecl->params.count; j++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);

        if (StrMapGet(&scope, p->name))
        {
            DiagErrorFmt(diag, p->base.range, "redefinition of '%s'", p->name);
        }

        StrMapPut(&scope, p->name, (void*)&p->type);

        if (p->type.isConst)
        {
            StrMapPut(&r->m_constVars, p->name, (void*)1);
        }

        if (AliasIsOwning(r, &p->type) && p->mod == ModRef)
        {
            StrMapPut(&r->m_refBoxParams, p->name, (void*)1);
        }

        /* A `ref T[]` binding may be a borrowed stack view (fixed-array
           slice): rebinds and growing writes would free caller storage. */
        if (p->mod == ModRef && TypeNameIsDynamicArray(&p->type))
        {
            StrMapPut(&r->m_refArrayParams, p->name, (void*)1);
        }
    }

    r->m_currentReturnType = &functionDecl->returnType;
    r->m_currentFunction = functionDecl;

    WalkBlock(r, (Block*)functionDecl->body, &scope);
    r->m_currentReturnType = NULL;
    r->m_currentFunction = NULL;

    CheckReturnPaths(r->m_diag, functionDecl);
    StrMapFree(&scope);
}

static void CheckFunctionBodies(Resolver* r)
{
    Module* mod = r->m_mod;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (!functionDecl->body)
        {
            continue;
        }

        ResolveFunctionBody(r, functionDecl);
    }
}

static void CheckFieldDefaults(Resolver* r)
{
    Module* mod = r->m_mod;

    StrMap defaultScope;
    StrMapInit(&defaultScope);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);
        StrMapPut(&defaultScope, gd->name, (void*)&gd->type);
    }

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (sd->incomplete || sd->isTypeAlias)
        {
            continue;
        }

        if (sd->isComponent)
        {
            CheckComponentFields(r, sd);
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (field->defaultValue)
            {
                ResolveFieldDefault(r, field, &defaultScope);
            }
        }
    }

    StrMapFree(&defaultScope);
}

static void CheckStructFields(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (sd->incomplete)
        {
            continue;
        }

        // Layout errors are computed in the registry; reported here for ranges.
        const StructType* registered = TypeRegistryFind(&r->m_registry, sd->name);

        if (registered && registered->layoutError)
        {
            DiagErrorFmt(diag, sd->base.range, "%s", registered->layoutError);
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (sd->isExtern
                && (TypeIsString(&r->m_registry, field->type.name) || ResolvesToDynamicArray(r, &field->type)))
            {
                // Strings and dynamic arrays are fats; extern structs must match C layout.
                if (TypeIsString(&r->m_registry, field->type.name))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "extern struct field '%s' may not have type 'string' "
                                 "(use 'cstring' for a raw const char*, or a '^byte' / integer-typed member)",
                                 field->name);
                }
                else
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "extern struct field '%s' may not have a dynamic array type ('%s'); "
                                 "extern structs must match C layout - use a fixed-size array or a pointer member",
                                 field->name, field->type.name);
                }
            }

            if (IsIncompleteStruct(&r->m_registry, field->type.name))
            {
                DiagErrorFmt(diag, field->type.range, "field '%s' has incomplete type '%s'", field->name,
                             field->type.name);
            }

            if (TypeNameHasArrayShape(&field->type) && field->type.length >= 0)
            {
                if (field->type.length < 1)
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' must have a length of at least 1", field->name);
                }

                ArrayLength total = 0;
                const TypeName* leaf = FixedArrayLeaf(&field->type, &total);

                if (TypeNameHasArrayShape(leaf))
                {
                    DiagErrorFmt(diag, field->type.range, "fixed-size array field '%s' may not contain a dynamic array",
                                 field->name);
                }
                else if (TypeNameIsBox(leaf) || AliasIsOwning(r, leaf))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' may not own its elements ('%s' is owning); "
                                 "fixed-size arrays have no drop glue",
                                 field->name, leaf->name);
                }
                else if (TypeRegistryIsOwningStruct(&r->m_registry, leaf->name))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' may not contain an owning struct ('%s'); "
                                 "fixed-size arrays have no drop glue",
                                 field->name, leaf->name);
                }
                else if (IsIncompleteStruct(&r->m_registry, leaf->name))
                {
                    DiagErrorFmt(diag, field->type.range, "field '%s' has incomplete type '%s'", field->name,
                                 leaf->name);
                }
            }
            else if (TypeTreeHasFixedArray(&field->type))
            {
                /* e.g. `int[4][]` — a dynamic array whose elements are fixed
                   arrays: not supported (fixed arrays live only in struct
                   fields, nested inside other fixed arrays). */
                DiagErrorFmt(diag, field->type.range,
                             "fixed-size arrays may only appear as (nested) fixed-size array struct fields, "
                             "not inside '%s'",
                             field->type.name);
            }
        }
    }
}

static void CheckGlobalInits(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    StrMap globalScope;
    StrMapInit(&globalScope);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);
        StrMapPut(&globalScope, gd->name, (void*)&gd->type);
    }

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (TypeTreeHasFixedArray(&gd->type))
        {
            DiagErrorFmt(diag, gd->base.range,
                         "global '%s' may not have a fixed-size array type ('%s'); "
                         "fixed-size arrays are only allowed as struct fields and locals",
                         gd->name, gd->type.name);
        }

        /* Mirrors the local rule: a const binding needs a value. */
        if (gd->type.isConst && !gd->init)
        {
            DiagErrorFmt(diag, gd->base.range, "const global '%s' must be initialized", gd->name);
        }

        /* Array globals default to an empty {null, 0} fat struct. */
        if (TypeNameIsDynamicArray(&gd->type))
        {
            if (gd->init)
            {
                ResolveExpr(r, gd->init, &globalScope);
                const TypeName* initType = InferType(r, gd->init, &globalScope);

                if (initType && strcmp(initType->name, gd->type.name) != 0)
                {
                    DiagErrorFmt(diag, gd->base.range,
                                 "global '%s' of type '%s' cannot be initialized by expression of type '%s'", gd->name,
                                 gd->type.name, initType->name);
                }
            }

            continue;
        }

        /* Scalar / alias / struct globals: the initializer is type-checked
           like a local declaration (the backend only lowers compile-time
           constant initializers, so nothing here may slip through). */
        if (!AliasIsOwning(r, &gd->type))
        {
            if (gd->init)
            {
                ResolveExpr(r, gd->init, &globalScope);
                const TypeName* initType = InferType(r, gd->init, &globalScope);

                if (initType && !IsAssignableType(r, &gd->type, initType)
                    && !IsStrLiteralForCString(r, &gd->type, gd->init))
                {
                    DiagErrorFmt(diag, gd->base.range,
                                 "global '%s' of type '%s' cannot be initialized by expression of type '%s'", gd->name,
                                 gd->type.name, initType->name);
                }
            }
            else if (!TypeClassify(&r->m_registry, r->m_arena, &gd->type).zeroValid)
            {
                /* A global starts as zeroes, and a zeroed box is a null pointer waiting to be read. */
                DiagErrorFmt(diag, gd->base.range,
                             "global '%s' of type '%s' must be initialized: it holds a box, which has no zero value",
                             gd->name, gd->type.name);
            }

            continue;
        }

        // String globals default to empty, like arrays.
        bool stringLike = TypeIsString(&r->m_registry, gd->type.name)
                          || (TypeNameIsOptional(&gd->type) && gd->type.inner
                              && TypeIsString(&r->m_registry, gd->type.inner->name));

        if (!gd->init)
        {
            if (!stringLike)
            {
                DiagErrorFmt(diag, gd->base.range, "box global '%s' must be initialized", gd->name);
            }

            continue;
        }

        const TypeName* boxInner = TypeNameBoxInner(&gd->type);

        /* An owning global can't be initialized by moving a source. */
        if (MovableBoxSourceKey(r, gd->init))
        {
            DiagErrorFmt(diag, gd->base.range,
                         "global '%s' cannot be initialized by moving from another variable; "
                         "initialize it with a value of '%s' or a call returning '%s'",
                         gd->name, gd->type.name, gd->type.name);
            continue;
        }

        ResolveExpr(r, gd->init, &globalScope);

        const TypeName* initType = InferType(r, gd->init, &globalScope);

        /* boxInner is NULL for `string` (no box inner), so only compare
            against it when present; otherwise the bare type must match. */
        bool ok = initType
                  && ((boxInner && strcmp(initType->name, boxInner->name) == 0)
                      || strcmp(initType->name, gd->type.name) == 0);

        if (initType && !ok)
        {
            DiagErrorFmt(diag, gd->base.range, "global '%s' cannot be initialized by expression of type '%s'", gd->name,
                         initType->name);
        }
    }

    StrMapFree(&globalScope);
}

static void CheckFunctionSignatures(Resolver* r)
{
    Module* mod = r->m_mod;
    DiagnosticEngine* diag = r->m_diag;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);

            if (TypeTreeHasFixedArray(&p->type))
            {
                DiagErrorFmt(diag, p->base.range,
                             "parameter '%s' may not have a fixed-size array type ('%s'); "
                             "fixed-size arrays are only allowed as struct fields and locals",
                             p->name, p->type.name);
            }

            /* By value, an owning struct argument is deep-copied for the callee: never for a destructor. */
            if (!functionDecl->isExtern && p->mod == ModNone && !TypeNameIsBox(&p->type)
                && !TypeNameIsOptional(&p->type) && !TypeNameHasArrayShape(&p->type)
                && TypeNameHasDrop(&r->m_registry, &p->type))
            {
                DiagErrorFmt(
                    diag, p->base.range,
                    "parameter '%s' cannot take '%s' by value: it has a Drop method; pass 'ref %s' or a box '^%s'",
                    p->name, p->type.name, p->type.name, p->type.name);
            }

            /* Structs (forward-declared ones included) cross to the host as pointers, so the
               declaration has to say so. */
            const char* paramLeaf = TypeRegistryResolveAlias(&r->m_registry, p->type.name);

            if (functionDecl->isExtern && p->mod == ModNone && TypeNameIsBare(&p->type)
                && (IsDefinedStruct(&r->m_registry, paramLeaf) || IsIncompleteStruct(&r->m_registry, paramLeaf)))
            {
                DiagErrorFmt(diag, p->base.range,
                             "extern parameter '%s' can't take struct '%s' by value; the host receives a pointer, "
                             "so declare it 'const ref %s' (or 'ref %s' if the host writes to it)",
                             p->name, p->type.name, p->type.name, p->type.name);
            }
        }

        if (TypeTreeHasFixedArray(&functionDecl->returnType))
        {
            DiagErrorFmt(diag, functionDecl->base.range,
                         "function '%s' may not return a fixed-size array type ('%s'); "
                         "fixed-size arrays are only allowed as struct fields and locals",
                         functionDecl->name, functionDecl->returnType.name);
        }

        /* Resolve aliases first: `struct Name = string;` returns a scalar
           pointer across the boundary, not a struct. A `return` param (out
           pointer) is exempt: its type comes back through the parameter,
           never in the return register. */
        if (functionDecl->isExtern && !functionDecl->hasReturnParam
            && IsDefinedStruct(&r->m_registry, TypeRegistryResolveAlias(&r->m_registry, functionDecl->returnType.name)))
        {
            DiagError(diag, functionDecl->base.range, "extern function cannot return a struct type by value");
        }

        /* Fat-pointer type returns are disallowed for externs. A bare `string`
           return IS allowed: it crosses as a NUL-terminated char* that the
           caller copies into a fresh owned buffer (codegen BuildOwnedStringFromCStr). */
        if (functionDecl->isExtern && !functionDecl->hasReturnParam)
        {
            const char* leaf = TypeRegistryResolveAlias(&r->m_registry, functionDecl->returnType.name);
            TypeName parsed = TypeNameParse(r->m_arena, leaf);

            const TypeName* unwrapped = TypeNameIsOptional(&parsed) ? parsed.inner : &parsed;
            bool isFat
                = (unwrapped && TypeNameIsDynamicArray(unwrapped))
                  || (TypeNameIsOptional(&parsed) && TypeIsString(&r->m_registry, unwrapped ? unwrapped->name : NULL));

            if (isFat)
            {
                DiagErrorFmt(diag, functionDecl->base.range,
                             "extern function cannot return '%s' by value; the {data, len} fat has no safe C "
                             "return ABI - declare a `return` out-param instead: 'extern void %s(return %s out)'",
                             functionDecl->returnType.name, functionDecl->name, functionDecl->returnType.name);
            }
        }

        /* A `return` param (out pointer) is exempt: its type comes back
           through the pointer, which needs no size - exactly like a `ref Foo`
           parameter. The CALLER still needs a defined type to allocate the
           out slot, so the call site requires it (see ResolveCall). */
        if (!functionDecl->hasReturnParam && IsIncompleteStruct(&r->m_registry, functionDecl->returnType.name))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "function cannot return incomplete type '%s'",
                         functionDecl->returnType.name);
        }
    }
}

static void ReleaseResolverState(Resolver* r)
{
    StrMapFree(&r->m_constVars);
    StrMapFree(&r->m_constGlobals);
    StrMapFree(&r->m_boxGlobals);
    StrMapFree(&r->m_refBoxParams);
    StrMapFree(&r->m_refLocals);
    StrMapFree(&r->m_refArrayParams);
    StrMapFree(&r->m_typeCache);
    free(r->m_deferFrames.items);
    free(r->m_loopFlows.items);
}

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Resolver r = {0};
    r.m_mod = mod;
    r.m_diag = diag;
    r.m_arena = arena;

    CollectDeclarations(&r);
    RegisterTypeAliases(&r);
    ResolveEnumDecls(&r);
    FoldManifestConstants(&r);
    ResolveStructDims(&r);
    BuildRegistryAndImpls(&r);
    CheckUnionFields(&r);
    InitResolverState(&r);
    MangleFunctions(&r);
    CheckDeclaredFunctions(&r);
    CheckFunctionBodies(&r);
    CheckFieldDefaults(&r);
    CheckStructFields(&r);
    CheckGlobalInits(&r);
    CheckFunctionSignatures(&r);
    ReleaseResolverState(&r);

    MoveCheckModule(mod, &r.m_registry, &r.m_nodeTypes, diag, arena);

    NodeTypesFree(&r.m_nodeTypes);
    TypeRegistryFree(&r.m_registry);
}
