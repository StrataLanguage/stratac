/* Call resolution: overloads, intrinsics, generic externs, delegates and export checks. */
#include "Sema/Resolve/ResolverInternal.h"

static bool ResolveIntrinsicCall(Resolver* r, CallExpr* c, StrMap* scope);

/* Count of params a call actually passes arguments for: excludes the
   trailing typed-rest param and the implicit `return` out-param of externs. */
size_t NamedParamCount(const FunctionDecl* f)
{
    bool typedRest = f->isVariadic && !f->isCVararg;
    size_t n = typedRest && f->params.count > 0 ? f->params.count - 1 : f->params.count;

    if (FunctionHasReturnParam(f))
    {
        n -= 1;
    }

    return n;
}

static Str GenerateCallSignatureString(Resolver* r, CallExpr* c, StrMap* scope)
{
    int emittedLength = 0;

    Sb b;
    SbInit(&b);

    SbPrintf(&b, "%s(", c->callee);

    for (size_t i = 0; i < c->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, i);

        const TypeName* argType = InferType(r, arg, scope);

        if (!argType)
        {
            continue;
        }

        SbPrintf(&b, "%s%s", argType->name, (i < c->args.count - 1) ? ", " : "");
    }

    SbPutc(&b, ')');

    Str finalStr = SbCDup(&b);
    SbFree(&b);

    return finalStr;
}

bool CheckArgTypesUnordered(Resolver* r, CallExpr* c, StrMap* scope, const AllowList* allowed, const int allowedSize)
{
    for (int i = 0; i < allowedSize; i++)
    {
        const AllowList* al = &allowed[i];

        if (CheckAllowListSingle(r, c, scope, al->allowed, al->size))
        {
            return true;
        }
    }

    /* No valid format found, build an error message */

    const int formatBufferSize = 2048;
    char formatBuffer[2048]; /* literal: a VLA under MSVC */

    char* tmpBuffer = formatBuffer;

    int numFilteredSignatures = 0;
    for (int i = 0; i < allowedSize; i++)
    {
        const AllowList* al = &allowed[i];

        /* Only add the prototypes with the matching amount of arguments */
        if (al->size != c->args.count)
        {
            continue;
        }

        ++numFilteredSignatures;
    }

    int filteredIndex = 0;

    for (int i = 0; i < allowedSize; i++)
    {
        const AllowList* al = &allowed[i];

        /* Only add the prototypes with the matching amount of arguments */
        if (al->size != c->args.count)
        {
            continue;
        }

        int skip = snprintf(tmpBuffer, formatBufferSize, "%s(", c->callee);
        if (skip < 0)
        {
            break;
        }
        tmpBuffer += skip;

        for (int j = 0; j < al->size; j++)
        {
            const char* paramType = al->allowed[j];
            skip = snprintf(tmpBuffer, formatBufferSize, "%s%s", paramType, ((j < al->size - 1) ? ", " : ""));
            if (skip < 0)
            {
                break;
            }
            tmpBuffer += skip;
        }

        skip
            = snprintf(tmpBuffer, formatBufferSize, ")%s", ((filteredIndex < numFilteredSignatures - 1) ? " or " : ""));
        if (skip < 0)
        {
            break;
        }

        ++filteredIndex;

        tmpBuffer += skip;
    }

    Str callSig = GenerateCallSignatureString(r, c, scope);
    DiagErrorFmt(r->diag, c->base.range, "no matching call '%s'", callSig.data);
    free((void*)callSig.data);

    DiagNoteFmt(r->diag, c->base.range, "did you mean %s?", formatBuffer);

    return false;
}

/* Private functions are visible only in the file that defines them */
bool FunctionVisibleTo(const FunctionDecl* fd, uint16_t callerFile)
{
    return !fd->isPrivate || fd->base.range.fileId == callerFile;
}

/* Returns true if the module declares any user-defined function with `name`
   that is visible to `callerFile`.
   `dot`/`cross` are only intrinsic fallbacks when no user overload exists, so a
   user's `dot(Vec3, Vec3)` is never hijacked by the SIMD resolver. */
bool ModuleHasFunctionNamed(Resolver* r, const char* name, uint16_t callerFile)
{
    for (size_t i = 0; i < r->mod->functions.count; i++)
    {
        FunctionDecl* fd = (FunctionDecl*)VecGet(&r->mod->functions, i);
        if (strcmp(fd->name, name) == 0 && FunctionVisibleTo(fd, callerFile))
        {
            return true;
        }
    }
    return false;
}

//-- `extern delegate` types.

/* A delegate is called with the extern ABI, so its signature follows the extern rules. */
void CheckDelegateDecl(Resolver* r, const FunctionDecl* f)
{
    if (f->hasReturnParam)
    {
        DiagErrorFmt(r->diag, f->base.range, "delegate '%s' cannot take a 'return' parameter", f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&f->params, i);
        const char* leaf = TypeRegistryResolveAlias(&r->registry, p->type.name);

        if (p->mod == ModNone && TypeNameIsBare(&p->type) && IsDefinedStruct(&r->registry, leaf))
        {
            DiagErrorFmt(r->diag, p->base.range,
                         "delegate parameter '%s' can't take struct '%s' by value; declare it 'const ref %s'", p->name,
                         p->type.name, p->type.name);
        }

        if (p->isVarargRest)
        {
            DiagErrorFmt(r->diag, p->base.range, "delegate '%s' cannot have a typed rest parameter", f->name);
        }
    }

    const char* ret = TypeRegistryResolveAlias(&r->registry, f->returnType.name);

    if (IsDefinedStruct(&r->registry, ret) || TypeNameIsDynamicArray(&f->returnType)
        || TypeNameIsOptional(&f->returnType))
    {
        DiagErrorFmt(r->diag, f->base.range, "delegate '%s' cannot return '%s' by value", f->name, f->returnType.name);
    }
}

void CheckExportDecl(Resolver* r, const FunctionDecl* f)
{
    if (f->isExtern || !f->body)
    {
        DiagErrorFmt(r->diag, f->base.range,
                     "'@export' function '%s' needs a body (it is defined here, not by the host)", f->name);
        return;
    }

    if (f->isVariadic)
    {
        DiagErrorFmt(r->diag, f->base.range, "'@export' function '%s' cannot be variadic", f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&f->params, i);
        const char* leaf = TypeRegistryResolveAlias(&r->registry, p->type.name);

        /* `ref T` of a plain struct is a `T*` in C. */
        bool plainStructRef = p->mod == ModRef && IsDefinedStruct(&r->registry, leaf)
                              && !TypeRegistryIsOwningStruct(&r->registry, leaf);

        if (!plainStructRef && (p->mod == ModRef || !IsCAbiScalar(r, &p->type)))
        {
            DiagErrorFmt(r->diag, p->base.range,
                         "'@export' parameter '%s' of type '%s' has no C ABI; use numbers, bools, enums, handles, "
                         "cstring, or 'ref' to a plain struct",
                         p->name, p->type.name);
        }
    }

    if (GetPrimitiveType(f->returnType.name) != PrimVoid && !IsCAbiScalar(r, &f->returnType))
    {
        DiagErrorFmt(r->diag, f->base.range,
                     "'@export' function '%s' cannot return '%s'; return a number, bool, enum, handle or cstring",
                     f->name, f->returnType.name);
    }
}

// Move owned args into params; drop facts a real call could break.
/* A ref is a pointer to the place, so it cannot point at a field whose `fieldoffset` breaks its alignment. */
static void CheckMisalignedRef(Resolver* r, Node* arg, StrMap* scope)
{
    if (!arg || arg->kind != NodeMember)
    {
        return;
    }

    MemberExpr* m = (MemberExpr*)arg;
    const TypeName* baseType = UnwrapBoxPtr(InferType(r, m->baseNode, scope));

    if (baseType && baseType->name && TypeRegistryFieldMisaligned(&r->registry, baseType->name, m->member))
    {
        DiagErrorFmt(r->diag, arg->range,
                     "cannot take a ref to field '%s' of '%s': its fieldoffset is misaligned for its type; read or "
                     "write it by value",
                     m->member, baseType->name);
    }
}

/* Arguments bound to `ref` / `const ref` parameters are passed by address. */
static void CheckRefArgAlignment(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope)
{
    size_t named = NamedParamCount(best);

    for (size_t j = 0; j < c->args.count && j < named; j++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet(&best->params, j);

        if (p->mod == ModRef)
        {
            CheckMisalignedRef(r, (Node*)VecGet(&c->args, j), scope);
        }
    }
}

// A mutable `ref` param may write through its arg, so the arg must not be rooted at a `const` binding.
static void CheckConstRefArgs(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope)
{
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = NamedParamCount(best);

    for (size_t j = 0; j < c->args.count; j++)
    {
        const ParamDecl* rp = NULL;

        if (j < namedCount)
        {
            rp = (ParamDecl*)VecGet(&best->params, j);
        }
        else if (typedRest && best->params.count > 0)
        {
            rp = (ParamDecl*)VecGet(&best->params, best->params.count - 1);
        }

        if (!rp || rp->mod != ModRef || rp->type.isConst)
        {
            continue;
        }

        Node* arg = (Node*)VecGet(&c->args, j);
        const char* constName = ConstRootName(r, arg, scope);

        if (constName)
        {
            DiagErrorFmt(r->diag, arg->range,
                         "'%s' is immutable and cannot be passed to non-const 'ref' parameter '%s' of '%s'", constName,
                         rp->name ? rp->name : "", best->name);
        }
    }
}

// `T?` args unwrapped into plain targets must be proven non-empty.
void CheckCallArgOptionalDerefs(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->args.count == 0)
    {
        return;
    }

    const FunctionDecl* fd = c->resolvedDecl;

    if (fd)
    {
        bool typedRest = fd->isVariadic && !fd->isCVararg;
        size_t namedCount = NamedParamCount(fd);

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);
            const TypeName* at = InferType(r, arg, scope);

            const TypeName* target = NULL;

            if (j < namedCount)
            {
                target = &((ParamDecl*)VecGet(&fd->params, j))->type;
            }
            else if (typedRest)
            {
                const ParamDecl* restParam = (ParamDecl*)VecGet(&fd->params, fd->params.count - 1);
                target = TypeNameArrayElem(&restParam->type);
            }
            else if (fd->isCVararg)
            {
                target = TypeNameBoxInner(at);
            }
        }

        return;
    }

    // Struct constructor: field slots are the targets.
    if (TypeRegistryIsUserType(&r->registry, c->callee))
    {
        const StructType* st = TypeRegistryFind(&r->registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd2 = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);
        }

        return;
    }

    // array_push: the element slot is the target.
    if (c->callee && strcmp(c->callee, "array_push") == 0 && c->args.count == 2)
    {
        const TypeName* arrType = InferType(r, (Node*)VecGet(&c->args, 0), scope);
        const TypeName* elem
            = arrType && TypeNameIsOptional(arrType) ? TypeNameArrayElem(arrType->inner) : TypeNameArrayElem(arrType);
        Node* valArg = (Node*)VecGet(&c->args, 1);

        if (arrType && TypeNameIsOptional(arrType))
        {
            Node* arrArg = (Node*)VecGet(&c->args, 0);
        }
    }
}

static bool IsTypeParamName(const FunctionDecl* generic, const TypeName* type)
{
    return generic->typeParam && type && type->name && strcmp(type->name, generic->typeParam) == 0;
}

// True when T appears in `type`, directly or inside ^T / T? / T[] / T[N].
static bool TypeMentionsParam(const FunctionDecl* generic, const TypeName* type)
{
    for (const TypeName* at = type; at;
         at = (TypeNameIsBox(at) || TypeNameIsOptional(at)) ? at->inner : (TypeNameHasArrayShape(at) ? at->elem : NULL))
    {
        if (IsTypeParamName(generic, at))
        {
            return true;
        }
    }

    return false;
}

/* The `extern<T>` named `name` visible from `fileId`, or NULL. */
static FunctionDecl* FindGenericExtern(Resolver* r, const char* name, uint16_t fileId)
{
    for (size_t i = 0; i < r->mod->functions.count; i++)
    {
        FunctionDecl* candidate = (FunctionDecl*)VecGet(&r->mod->functions, i);

        if (candidate->typeParam && strcmp(candidate->name, name) == 0 && FunctionVisibleTo(candidate, fileId))
        {
            return candidate;
        }
    }

    return NULL;
}

/* `generic` with T replaced by `structName`, created once per type and kept on the module so
   codegen declares it. The instantiation keeps the generic's host symbol (`name`). */
static FunctionDecl* GetGenericInstance(Resolver* r, const FunctionDecl* generic, const char* structName)
{
    for (size_t i = 0; i < r->mod->genericInstances.count; i++)
    {
        FunctionDecl* existing = (FunctionDecl*)VecGet(&r->mod->genericInstances, i);

        if (existing->genericOf == generic && strcmp(existing->typeArg, structName) == 0)
        {
            return existing;
        }
    }

    FunctionDecl* instance = AST_NEW(r->arena, FunctionDecl);
    *instance = *generic;
    instance->typeParam = NULL;
    instance->genericOf = generic;
    instance->typeArg = ArenaStrdup(r->arena, structName);
    instance->mangledName = ArenaFormat(r->arena, "%s<%s>", generic->name, structName);
    VecInit(&instance->params);

    for (size_t i = 0; i < generic->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&generic->params, i);
        ParamDecl* copy = AST_NEW(r->arena, ParamDecl);
        *copy = *param;

        if (IsTypeParamName(generic, &param->type))
        {
            TypeName substituted = TypeNameParse(r->arena, instance->typeArg);
            substituted.isConst = param->type.isConst;
            substituted.range = param->type.range;
            copy->type = substituted;
        }

        VecPush(&instance->params, copy);
    }

    VecPush(&r->mod->genericInstances, instance);

    return instance;
}

/* What `extern<T>` allows: T only as a whole `ref` / `const ref` parameter type, never in the
   return type, and no variadic or `return` parameters. */
void CheckGenericExternDecl(Resolver* r, const FunctionDecl* f)
{
    if (TypeRegistryIsUserType(&r->registry, f->typeParam) || GetPrimitiveType(f->typeParam) != PrimNone)
    {
        DiagErrorFmt(r->diag, f->base.range, "type parameter '%s' of '%s' has the same name as a type", f->typeParam,
                     f->name);
    }

    if (TypeMentionsParam(f, &f->returnType))
    {
        DiagErrorFmt(r->diag, f->base.range, "generic extern '%s' can't return its type parameter '%s'", f->name,
                     f->typeParam);
    }

    if (f->isVariadic || f->hasReturnParam)
    {
        DiagErrorFmt(r->diag, f->base.range, "generic extern '%s' can't be variadic or take a 'return' parameter",
                     f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&f->params, i);

        if (TypeMentionsParam(f, &param->type) && (!IsTypeParamName(f, &param->type) || param->mod != ModRef))
        {
            DiagErrorFmt(r->diag, param->base.range,
                         "parameter '%s' of generic extern '%s' must be 'const ref %s' or 'ref %s'", param->name,
                         f->name, f->typeParam, f->typeParam);
        }
    }
}

/* A call to an `extern<T>`: infers or reads T, instantiates the extern for it, and checks the
   arguments against the instantiation. Returns false when the callee isn't generic. */
static bool ResolveGenericExternCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->resolvedDecl)
    {
        return false;
    }

    FunctionDecl* generic = FindGenericExtern(r, c->callee, c->base.range.fileId);

    if (!generic)
    {
        if (c->typeArg && !IsTypedMemoryIntrinsic(r, c))
        {
            DiagErrorFmt(r->diag, c->base.range, "'%s' is not a generic extern; it takes no type argument", c->callee);
            return true;
        }

        return false;
    }

    const TypeName* typeArg = c->typeArg;

    for (size_t i = 0; !typeArg && i < generic->params.count && i < c->args.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&generic->params, i);

        if (IsTypeParamName(generic, &param->type))
        {
            typeArg = InferType(r, (Node*)VecGet(&c->args, i), scope);
        }
    }

    if (!typeArg)
    {
        DiagErrorFmt(r->diag, c->base.range, "can't infer '%s' for '%s'; name it explicitly: '%s<Type>(...)'",
                     generic->typeParam, generic->name, generic->name);
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->registry, typeArg->name);
    const StructType* structType
        = (TypeNameHasArrayShape(typeArg) || TypeNameIsBox(typeArg) || TypeNameIsOptional(typeArg))
              ? NULL
              : TypeRegistryFind(&r->registry, leaf);

    if (!structType || structType->opaque || structType->isTypeAlias)
    {
        DiagErrorFmt(r->diag, c->base.range, "type argument '%s' for '%s' must be a struct type", typeArg->name,
                     generic->name);
        return true;
    }

    if (structType->owning)
    {
        DiagErrorFmt(r->diag, c->base.range,
                     "type argument '%s' for '%s' must be plain data; it holds owning fields (strings, boxes, "
                     "optionals or dynamic arrays)",
                     typeArg->name, generic->name);
        return true;
    }

    FunctionDecl* instance = GetGenericInstance(r, generic, structType->name);
    c->callee = instance->mangledName;
    c->resolvedDecl = instance;

    if (NamedParamCount(instance) != c->args.count)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' takes %zu argument(s) but %zu were given", generic->name,
                     NamedParamCount(instance), c->args.count);
        return true;
    }

    for (size_t i = 0; i < c->args.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&instance->params, i);
        Node* arg = (Node*)VecGet(&c->args, i);
        const TypeName* argType = InferType(r, arg, scope);

        if (param->mod == ModRef && !param->type.isConst && !IsLValueNode(arg))
        {
            DiagErrorFmt(r->diag, arg->range,
                         "argument %zu of '%s' is passed by non-const 'ref' and must be a variable", i + 1,
                         generic->name);
        }
        else if (argType && !IsAssignableType(r, &param->type, argType))
        {
            DiagErrorFmt(r->diag, arg->range, "argument %zu of '%s' has type '%s' but '%s' was expected", i + 1,
                         generic->name, argType->name, param->type.name);
        }
    }

    CheckCallArgOptionalDerefs(r, c, scope);
    CheckConstRefArgs(r, instance, c, scope);
    CheckRefArgAlignment(r, instance, c, scope);
    TrackCallArgMoves(r, instance, c);

    return true;
}

/* A call through a value of an `extern delegate` type: `fn(args)` names a variable, `obj.field(args)` a
   struct field. Fills c->delegateCallee / c->delegateDecl; false when the call is something else. */
static bool DetectDelegateCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->delegateDecl)
    {
        return true;
    }

    if (!c->callee)
    {
        return false;
    }

    if (c->calleeBase)
    {
        const TypeName* baseType = UnwrapBoxPtr(InferType(r, c->calleeBase, scope));
        const StructType* st = baseType ? TypeRegistryFind(&r->registry, baseType->name) : NULL;
        int idx = st ? TypeRegistryFieldIndex(&r->registry, baseType->name, c->callee) : -1;

        if (idx < 0)
        {
            return false;
        }

        const FieldDecl* field = (const FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx);

        if (!TypeRegistryDelegate(&r->registry, field->type.name))
        {
            return false;
        }

        MemberExpr* m = AST_NEW(r->arena, MemberExpr);
        m->base.kind = NodeMember;
        m->base.range = c->base.range;
        m->baseNode = c->calleeBase;
        m->member = c->callee;
        c->delegateCallee = (Node*)m;
        c->calleeBase = NULL;
    }
    else
    {
        const TypeName* varType = (const TypeName*)StrMapGet(scope, c->callee);

        if (!varType || !TypeRegistryDelegate(&r->registry, varType->name))
        {
            return false;
        }

        IdentExpr* id = AST_NEW(r->arena, IdentExpr);
        id->base.kind = NodeIdent;
        id->base.range = c->base.range;
        id->name = c->callee;
        c->delegateCallee = (Node*)id;
    }

    ResolveExpr(r, c->delegateCallee, scope);

    const TypeName* t = InferType(r, c->delegateCallee, scope);
    c->delegateDecl = t ? TypeRegistryDelegate(&r->registry, t->name) : NULL;

    if (!c->delegateDecl)
    {
        return false;
    }

    c->callee = c->delegateDecl->mangledName;
    return true;
}

/* `expr.Member(args)` / `Type.Static(args)` — resolve against impl blocks.
   On success the call is rewritten to the extern symbol (the receiver is
   prepended as the first argument for instance methods) and resolution
   falls through to the normal overload machinery. */
static bool ResolveCalleeLookup(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->calleeBase && ResolveMemberCall(r, c, scope))
    {
        return true;
    }

    return ResolveGenericExternCall(r, c, scope);
}

/* A whole fixed-size array *local* cannot be passed as a call argument —
   only its elements (`values[i]`, `values.length`) — UNLESS some overload
   borrows it as a `ref T[]` slice/view (a stack fat is built at the call
   site; the callee never drops the binding). Locals are named by a bare
   identifier, so an Ident arg with a fixed type is always a local;
   field-sourced fixed arrays (`b.data`) keep the extern `^T` decay below. */
static bool RejectFixedArrayArgs(Resolver* r, CallExpr* c, StrMap* scope)
{
    DecayFixedArgsToPtr(r, c, scope);

    bool fixedArgError = false;

    for (size_t i = 0; i < c->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, i);

        if (arg->kind != NodeIdent)
        {
            continue;
        }

        const TypeName* argType = InferType(r, arg, scope);

        /* Taking the address of a fixed array is what `addressof` is for. */
        bool addressOf = c->callee && strcmp(c->callee, "addressof") == 0
                         && !ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId);

        if (argType && TypeNameIsFixedArray(argType) && !addressOf
            && !OverloadTakesFixedView(r, c->callee, argType, c->base.range.fileId))
        {
            DiagErrorFmt(r->diag, arg->range,
                         "cannot pass fixed-size array '%s' of type '%s' as an argument; "
                         "pass its elements instead (e.g. '%s[0]')",
                         ((IdentExpr*)arg)->name, argType->name, ((IdentExpr*)arg)->name);
            fixedArgError = true;
        }
    }

    return fixedArgError;
}

/* Struct constructor call: owning fields from owning sources move, and braced
   arguments take the field's struct or array shape. Returns false when the callee
   is not a struct type. */
static bool ResolveStructCtorCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!TypeRegistryIsUserType(&r->registry, c->callee))
    {
        return false;
    }

    const StructType* st = TypeRegistryFind(&r->registry, c->callee);

    for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
    {
        FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
        Node* arg = (Node*)VecGet(&c->args, j);

        bool bracedArg = (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
                         || (arg->kind == NodeStructInit && !((StructInitExpr*)arg)->typeName);

        // A braced arg takes the field's struct/array shape.
        Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

        if (!bracedArg && resolvedArg == arg)
        {
            resolvedArg = arg;
        }

        if (resolvedArg != arg)
        {
            VecSet(&c->args, j, resolvedArg);
        }

        if (bracedArg && resolvedArg->kind == NodeStructInit && ((StructInitExpr*)resolvedArg)->typeName
            && !IsEmptyBracedForOptional(resolvedArg, &fd->type))
        {
            CheckStructInitFields(r, resolvedArg);
        }
        else if (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
        {
            // Braced array field: fill the element type and move-mark.
            const TypeName* ft = &fd->type;
            const TypeName* ftArr = TypeNameIsOptional(ft) ? ft->inner : ft;

            if (TypeNameIsDynamicArray(ftArr))
            {
                const TypeName* elem = TypeNameArrayElem(ftArr);
                ArrayInitExpr* ai = (ArrayInitExpr*)arg;

                ai->elementType = elem;

                if (elem && AliasIsOwningValue(r, elem))
                {
                    for (size_t k = 0; k < ai->elements.count; k++)
                    {
                        Node* elemNode = (Node*)VecGet(&ai->elements, k);
                        const char* movedKey = MovableBoxSourceKey(r, elemNode);

                        if (movedKey)
                        {
                            MarkMoved(r, movedKey, elemNode->range);
                        }
                    }
                }
            }
        }
    }

    // Check facts before moves clear them.
    CheckCallArgOptionalDerefs(r, c, scope);

    for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
    {
        FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
        Node* arg = (Node*)VecGet(&c->args, j);

        if (AliasIsOwningValue(r, &fd->type))
        {
            const char* movedKey = MovableBoxSourceKey(r, arg);

            if (movedKey)
            {
                MarkMoved(r, movedKey, arg->range);
            }
        }
    }

    return true;
}

/* Braced arg against a struct param is a positional struct init; rewrite before checks. */
static void RewriteBracedCallArgs(Resolver* r, CallExpr* c, FunctionDecl* best)
{
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = typedRest && best->params.count > 0 ? best->params.count - 1 : best->params.count;

    for (size_t j = 0; j < c->args.count && j < namedCount; j++)
    {
        Node* arg = (Node*)VecGet(&c->args, j);

        if (arg->kind != NodeArrayInit && arg->kind != NodeStructInit)
        {
            continue;
        }

        bool bracedArg = (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
                         || (arg->kind == NodeStructInit && !((StructInitExpr*)arg)->typeName);

        const ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);

        /* Braced array literal against a dynamic-array param: stamp the
           element types (the assign path does the same) so the literal
           constructs. A `ref T[]` binding BORROWS a stack-constructed
           temp; a by-value param takes ownership of a normal heap array
           the callee drops. Extern by-value arrays decay to T* — the
           host could never free a Strata temp, so literals are
           rejected there. */
        if (arg->kind == NodeArrayInit && TypeNameIsDynamicArray(&param->type) && !((ArrayInitExpr*)arg)->elementType)
        {
            if (best->isExtern && param->mod != ModRef)
            {
                DiagErrorFmt(r->diag, arg->range,
                             "cannot pass an array literal to by-value extern array parameter '%s' of '%s'; "
                             "pass an array variable or declare the parameter as 'ref %s'",
                             param->name ? param->name : "", best->name, param->type.name);
                continue;
            }

            StampArrayInitElemTypes((ArrayInitExpr*)arg, TypeNameArrayElem(&param->type));
        }

        Node* resolvedArg = ApplyBracedStructTarget(r, arg, &param->type);

        if (resolvedArg != arg)
        {
            VecSet(&c->args, j, resolvedArg);
        }

        if (bracedArg && resolvedArg->kind == NodeStructInit && ((StructInitExpr*)resolvedArg)->typeName
            && !IsEmptyBracedForOptional(resolvedArg, &param->type))
        {
            CheckStructInitFields(r, resolvedArg);
        }
    }
}

/* Facts before moves: the checks read what the call would move, so they run first. */
static void CheckResolvedCallArgs(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope)
{
    CheckCallArgOptionalDerefs(r, c, scope);
    CheckConstRefArgs(r, best, c, scope);
    CheckRefArgAlignment(r, best, c, scope);
    CheckGrowableArrayArgs(r, best, c, scope);
    TrackCallArgMoves(r, best, c);
}

/* Result checks for the chosen overload: its return type, braced arguments, then the
   per-argument facts and moves. */
static void FinishResolvedCall(Resolver* r, CallExpr* c, StrMap* scope, FunctionDecl* best)
{
    /* A `return` param's caller allocates the out slot, so the return type
       must be a defined (sized) struct at every call site. The declaration
       itself may use a forward-declared type; only the call requires it. */
    if (FunctionHasReturnParam(best) && IsIncompleteStruct(&r->registry, best->returnType.name))
    {
        DiagErrorFmt(r->diag, c->base.range, "call to '%s' has incomplete return type '%s'", best->name,
                     best->returnType.name);
        return;
    }

    RewriteBracedCallArgs(r, c, best);
    CheckResolvedCallArgs(r, best, c, scope);
}

/* Call already resolved (e.g. warmup): reuse the cached decl and rerun the checks and
   move tracking. Returns false when the cache does not apply. */
static bool ReuseCachedOverload(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->resolvedDecl)
    {
        return false;
    }

    const FunctionDecl* best = c->resolvedDecl;

    /* A cached `@private` decl must not leak across files. */
    if (!FunctionVisibleTo(best, c->base.range.fileId))
    {
        c->resolvedDecl = NULL;
        return false;
    }

    CheckResolvedCallArgs(r, best, c, scope);
    return true;
}

/* A call needs one visible candidate with its name (a delegate call has its own). */
static bool HasVisibleCandidate(const CallExpr* c, const Vec* pool, bool delegateCall)
{
    if (delegateCall)
    {
        return true;
    }

    for (size_t i = 0; i < pool->count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet((Vec*)pool, i);

        if (strcmp(functionDecl->name, c->callee) == 0 && FunctionVisibleTo(functionDecl, c->base.range.fileId))
        {
            return true;
        }
    }

    return false;
}

/* Arity filter: a variadic candidate needs its fixed params, any other needs an exact count. */
static bool CandidateArityMatches(const CallExpr* c, const FunctionDecl* functionDecl)
{
    if (functionDecl->isVariadic)
    {
        /* Extern `...` adds no param; a typed rest counts as one, so min args is one fewer. */
        size_t minArgs = functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1;

        return c->args.count >= minArgs;
    }

    return NamedParamCount(functionDecl) == c->args.count;
}

/* Scores an argument against a box, owned or view parameter (array decay at the extern boundary, optional smart casts,
 * `ref T[]` views, owned tails). Anything else is not viable. */
static bool ScoreOwnedOrViewArgument(const TypeName* argType, const TypeName* paramType, const ParamDecl* param,
                                     bool isTail, bool mutableRef, bool isExtern, int* score)
{
    if (isExtern && TypeNameIsOwning(paramType) && (TypeNameIsDynamicArray(argType) || TypeNameIsFixedArray(argType)))
    {
        /* Array decay at the extern boundary: a T[] / T[N] argument
           satisfies a ^T param by passing a pointer to its first
           element (the host sees T*). Checked before the generic
           owning-arg rule: dynamic arrays are owning too. */
        const TypeName* paramInner = TypeNameBoxInner(paramType);
        const TypeName* argElem = TypeNameArrayElem(argType);

        if (paramInner && argElem && strcmp(paramInner->name, argElem->name) == 0)
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else if (!mutableRef && TypeNameIsBox(paramType) && TypeNameIsOptional(argType) && TypeNameBoxInner(paramType)
             && TypeNameBoxInner(argType)
             && strcmp(TypeNameBoxInner(paramType)->name, TypeNameBoxInner(argType)->name) == 0)
    {
        /* Smart cast: a proven `T?` passes as `^T` (proof checked by
           CheckCallArgOptionalDerefs once the overload is picked). */
        *score += 1;
    }
    else if (TypeNameIsOwning(argType))
    {
        /* ^T coerces to T (implicit deref). */
        const TypeName* argInner = TypeNameBoxInner(argType);

        if (argInner && strcmp(argInner->name, paramType->name) == 0)
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else if (!isTail && param->mod == ModRef && TypeNameIsDynamicArray(paramType) && TypeNameIsFixedArray(argType))
    {
        /* Fixed → `ref T[]` slice/view: the fixed inline storage is
           borrowed as a stack fat {&fixed[0], N, N} — no copy, no
           allocation; a ref callee mutates elements, never drops or
           frees the binding (ref params are exempt from teardown). */
        const TypeName* argElem = TypeNameArrayElem(argType);
        const TypeName* paramElem = TypeNameArrayElem(paramType);

        if (argElem && paramElem && strcmp(argElem->name, paramElem->name) == 0)
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else if (isTail && param->mod == ModNone && TypeNameIsOwning(paramType))
    {
        const TypeName* paramInner = TypeNameBoxInner(paramType);

        // Owned rest tails box `T` into `^T` inline.
        if (paramInner && strcmp(paramInner->name, argType->name) == 0)
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    return true;
}

/* Scores one argument against one candidate: exact and implicit conversions add to the score, a mismatch rules the
 * candidate out. */
static bool ScoreArgument(Resolver* r, CallExpr* c, StrMap* scope, FunctionDecl* functionDecl, size_t j, int* score)
{
    Node* arg = (Node*)VecGet(&c->args, j);
    const TypeName* argType = InferType(r, arg, scope);

    if (!argType)
    {
        return true;
    }

    // Extra args collect into the rest tail.
    bool isTail = functionDecl->isVariadic
                  && j >= (functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1);

    if (functionDecl->isCVararg && isTail)
    {
        if (!IsCVarargCompatible(r, argType))
        {
            return false;
        }

        // Prefer exact-arity overloads over varargs.
        *score += 2;
        return true;
    }

    const ParamDecl* param = (ParamDecl*)VecGet(&functionDecl->params, isTail ? functionDecl->params.count - 1 : j);

    const TypeName* paramType = &param->type;

    if (isTail)
    {
        // Typed rest compares against the element type.
        const TypeName* elem = TypeNameArrayElem(paramType);

        if (elem)
        {
            paramType = elem;
        }
    }

    /* A mutable `ref` binds the caller's storage itself: a converted
       temporary would silently lose the callee's writes (and a wider
       handle/optional slot could be written with a value the
       caller's type cannot hold). Only exact types, box contents and
       fixed-array views bind. */
    bool mutableRef = !isTail && param->mod == ModRef && !paramType->isConst;

    if (strcmp(argType->name, paramType->name) == 0)
    {
    }
    else if (mutableRef && !SameResolvedType(r, argType->name, paramType->name)
             && !(TypeNameIsOwning(argType) && !TypeNameIsOptional(paramType)) && !TypeNameIsFixedArray(argType))
    {
        return false;
    }
    else if (arg->kind == NodeStrLiteral && TypeIsCString(&r->registry, paramType->name))
    {
        /* String literal contextually coerces to `cstring`. */
        *score += 1;
    }
    else if (TypeIsCString(&r->registry, argType->name) && TypeIsString(&r->registry, paramType->name))
    {
        /* `cstring` copies into a `string` param (heap copy). */
        *score += 1;
    }
    else if (!mutableRef && IsImplicitScalarConversion(paramType->primitiveType, argType->primitiveType)
             && ArgConvertsWithoutLoss(arg, argType, paramType))
    {
        *score += 1;
    }
    else if (IsSimdVector(argType->primitiveType) && IsSimdVector(paramType->primitiveType))
    {
        // Same rule as assignment: no implicit lane-count or element-type change.
        if (IsVectorAssignableType(r, paramType, argType, TypeRegistryResolveAlias(&r->registry, paramType->name),
                                   TypeRegistryResolveAlias(&r->registry, argType->name)))
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else if (HandleExtendsFrom(&r->registry, argType->name, paramType->name))
    {
        *score += 1;
    }
    else if (TypeNameIsOptional(paramType))
    {
        // `T` and `^T` coerce into `T?` when inners match.
        const char* paramInner = TypeNameBoxInner(paramType) ? TypeNameBoxInner(paramType)->name : NULL;

        bool optMatch = false;

        if (paramInner && (TypeNameIsBox(argType) || TypeNameIsOptional(argType)))
        {
            const TypeName* argInner = TypeNameBoxInner(argType);

            optMatch = argInner != NULL ? strcmp(argInner->name, paramInner) == 0
                                        : strcmp("string", paramInner) == 0; /* bare ^string */
        }
        else if (paramInner)
        {
            optMatch = strcmp(argType->name, paramInner) == 0;
        }

        if (optMatch)
        {
            *score += 1;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return ScoreOwnedOrViewArgument(argType, paramType, param, isTail, mutableRef, functionDecl->isExtern, score);
    }

    return true;
}

/* Scores a candidate over all arguments. Returns false when it is not viable. */
static bool ScoreCandidate(Resolver* r, CallExpr* c, StrMap* scope, FunctionDecl* functionDecl, int* scoreOut)
{
    int score = 0;

    /* Variadic candidates are a lower-priority fallback. */
    if (functionDecl->isVariadic)
    {
        score += 1;
    }

    for (size_t j = 0; j < c->args.count; j++)
    {
        if (!ScoreArgument(r, c, scope, functionDecl, j, &score))
        {
            return false;
        }
    }

    *scoreOut = score;
    return true;
}

/* Picks the best-scoring viable candidate. A tie with the best makes the call ambiguous. */
static FunctionDecl* SelectBestOverload(Resolver* r, CallExpr* c, StrMap* scope, const Vec* pool, bool delegateCall,
                                        bool* ambiguousOut)
{
    FunctionDecl* best = NULL;

    int bestScore = INT_MAX;
    bool ambiguous = false;

    for (size_t i = 0; i < pool->count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet((Vec*)pool, i);

        if (!delegateCall
            && (strcmp(functionDecl->name, c->callee) != 0 || !FunctionVisibleTo(functionDecl, c->base.range.fileId)))
        {
            continue;
        }

        if (!CandidateArityMatches(c, functionDecl))
        {
            continue;
        }

        int score = 0;

        if (!ScoreCandidate(r, c, scope, functionDecl, &score))
        {
            continue;
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = functionDecl;
            ambiguous = false;
        }
        else if (score == bestScore && best)
        {
            ambiguous = true;
        }
    }

    *ambiguousOut = ambiguous;
    return best;
}

/* Overload selection: a cached decl from an earlier pass, or the best viable candidate. */
static void ResolveOverloadCall(Resolver* r, CallExpr* c, StrMap* scope, bool delegateCall)
{
    if (ReuseCachedOverload(r, c, scope))
    {
        return;
    }

    /* A delegate call has exactly one candidate: its signature. */
    Vec delegatePool;
    VecInit(&delegatePool);

    if (delegateCall)
    {
        VecPush(&delegatePool, (void*)c->delegateDecl);
    }

    const Vec* pool = delegateCall ? &delegatePool : &r->mod->functions;

    if (!HasVisibleCandidate(c, pool, delegateCall))
    {
        DiagErrorFmt(r->diag, c->base.range, "unknown function '%s'", c->callee);
        free(delegatePool.items);
        return;
    }

    bool ambiguous = false;
    FunctionDecl* best = SelectBestOverload(r, c, scope, pool, delegateCall, &ambiguous);

    free(delegatePool.items);

    if (!best)
    {
        if (delegateCall)
        {
            DiagErrorFmt(r->diag, c->base.range, "arguments do not match delegate '%s'", c->delegateDecl->name);
        }
        else
        {
            DiagErrorFmt(r->diag, c->base.range, "no matching overload for '%s' with %zu argument(s)", c->callee,
                         c->args.count);
        }
        return;
    }

    if (ambiguous)
    {
        DiagErrorFmt(r->diag, c->base.range, "ambiguous call to overload '%s'", c->callee);
    }

    c->callee = best->mangledName;
    c->resolvedDecl = best;

    // Reported ambiguity: stop before side effects cause follow-on errors.
    if (ambiguous)
    {
        return;
    }

    FinishResolvedCall(r, c, scope, best);
}

void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    /* Calls through delegate values pick their one signature directly. */
    bool delegateCall = DetectDelegateCall(r, c, scope);

    if (!delegateCall)
    {
        if (ResolveCalleeLookup(r, c, scope))
        {
            return;
        }

        if (RejectFixedArrayArgs(r, c, scope))
        {
            return;
        }

        if (ResolveStructCtorCall(r, c, scope))
        {
            return;
        }

        if (ResolveIntrinsicCall(r, c, scope))
        {
            return;
        }
    }

    ResolveOverloadCall(r, c, scope, delegateCall);
}

// -- Instrinsics

// NOTE: Make sure there are matching definitions for any new intrinsics added at `EmitIntrinsicCall()` in
// LLVMModuleBuilder.c.
static const IntrinsicTypeDefinition intrinsics[] = {
    /* Array calls */
    {"array_push",        ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_pop",         ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_resize",      ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_remove",      ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_reserve",     ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_swap_remove", ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_replace",     ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_take",        ArrayBuiltinType,           ResolveArrayBuiltin    },

    /* Memory */
    {"copy",              CopyBuiltinType,            ResolveCopyBuiltin     },
    {"drop",              DropBuiltinType,            ResolveDropBuiltin     },
    {"addressof",         MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"sizeof",            MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"alignof",           MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"load",              MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"store",             MemoryBuiltinType,          ResolveMemoryBuiltin   },

    /* Strings */
    {"substring",         SubstringBuiltinType,       ResolveSubstringBuiltin},

    /* Vectors */
    {"float2",            VectorConstructBuiltinType, ResolveVectorConstruct },
    {"float3",            VectorConstructBuiltinType, ResolveVectorConstruct },
    {"float4",            VectorConstructBuiltinType, ResolveVectorConstruct },

    {"dot",               VectorDotBuiltinType,       ResolveVector2Arg      },
    {"cross",             VectorCrossBuiltinType,     ResolveVector2Arg      },
    {"reduce",            VectorReduceBuiltinType,    ResolveVector1Arg      },
};

static bool ResolveIntrinsicCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (c->callee == NULL)
    {
        return false;
    }

    for (int i = 0; i < ARRAY_COUNT(intrinsics); i++)
    {
        const IntrinsicTypeDefinition* intrinsic = &intrinsics[i];

        if (strcmp(intrinsic->name, c->callee) == 0)
        {
            bool result = intrinsic->resolveFunc(r, c, scope);

            // This call expr was resolved as a call to an intrinsic, mark and return true
            if (result)
            {
                // Mark this call expression as an intrinsic call
                c->isIntrinsicCall = true;
                return true;
            }
        }
    }

    return false;
}

const TypeName* InferIntrinsicType(Resolver* r, CallExpr* c, StrMap* scope)
{
    for (int i = 0; i < ARRAY_COUNT(intrinsics); i++)
    {
        const IntrinsicTypeDefinition* intrinsic = &intrinsics[i];

        if (strcmp(intrinsic->name, c->callee) == 0)
        {
            const TypeName* result = intrinsic->typeFunc(r, c, scope);
            if (result != NULL)
            {
                return result;
            }
        }
    }

    return NULL;
}

void ResolveCallExpr(Resolver* r, Node* n, StrMap* scope)
{
    CallExpr* c = (CallExpr*)n;

    /* `addressof(fn)`'s argument names a function, not a value. */
    bool argIsFunction = IsAddressOfFunctionArg(r, c, scope);

    for (size_t i = 0; i < c->args.count && !argIsFunction; i++)
    {
        ResolveExpr(r, (Node*)VecGet(&c->args, i), scope);
    }

    ResolveCall(r, c, scope);

    return;
}
