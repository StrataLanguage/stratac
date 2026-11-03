// Monomorphize: expands generic structs, impls and functions into the concrete declarations that use them, and
// folds every meta call (`@name(args)`) into a literal. Sema and codegen only ever see ordinary declarations: the
// struct `number<int>`, its impl, and the function `make_number<int>`. See docs/generics-plan.md, phase 3, and
// docs/meta-functions-plan.md, phase 2.
#include "Sema/Monomorphize.h"

#include "AST/ASTClone.h"

#include <stdlib.h>
#include <string.h>

// Past these, a generic type is taken to be recursive (`struct list<T> { list<list<T>>? next; }`) and the
// expansion stops with an error. The name cap stops spellings that double in length at each level.
#define MONO_MAX_DEPTH 32
#define MONO_MAX_NAME 1024
#define MONO_MAX_INSTANCES 4096

typedef struct
{
    Module* mod;
    DiagnosticEngine* diag;
    Arena* arena;
    StrMap structTemplates;   // name -> StructDecl*
    StrMap functionTemplates; // name -> FunctionDecl*
    StrMap structInstances;   // canonical name -> StructDecl*
    StrMap structDepths;      // canonical name -> nesting depth of the struct instance, as a pointer-sized integer
    StrMap functionInstances; // canonical name -> FunctionDecl*
    Vec implTemplates;        // ImplDecl*
    Vec pending;              // Node*: concrete declarations whose types are not rewritten yet
    StrMap metaFunctions;     // name -> MetaFunc*
    size_t metaCalls;         // meta calls folded so far: a condition that held one may fold to a constant
    size_t instanceCount;
    bool stopped; // an error ended the expansion: no further instances are made
} Mono;

static size_t MaxSize(size_t a, size_t b)
{
    return a > b ? a : b;
}

static void FreeVec(Vec* v)
{
    free(v->items);
    VecInit(v);
}

static bool VecHas(const Vec* v, const void* item)
{
    for (size_t i = 0; i < v->count; i++)
    {
        if (VecGet(v, i) == item)
        {
            return true;
        }
    }

    return false;
}

static void PushAll(Vec* dst, const Vec* src)
{
    for (size_t i = 0; i < src->count; i++)
    {
        VecPush(dst, VecGet(src, i));
    }
}

// The plain (non-generic) struct named `name`, or NULL. Only meaningful once the templates are moved out.
static const StructDecl* PlainStructNamed(const Module* mod, const char* name)
{
    for (size_t i = 0; i < mod->structs.count; i++)
    {
        const StructDecl* sd = (const StructDecl*)VecGet(&mod->structs, i);

        if (!sd->isTypeAlias && strcmp(sd->name, name) == 0)
        {
            return sd;
        }
    }

    return NULL;
}

// Room for one more instance of the template `base`. Past the limit the expansion stops at this use.
static bool TakeInstanceSlot(Mono* m, const char* base, SourceRange range)
{
    if (m->stopped)
    {
        return false;
    }

    if (m->instanceCount >= MONO_MAX_INSTANCES)
    {
        DiagErrorFmt(m->diag, range, "too many generic instances of '%s': recursive generic type?", base);
        m->stopped = true;
        return false;
    }

    m->instanceCount++;

    return true;
}

// Reports a use of the template `base` nested deeper than the limit, or spelled longer than it; false (and the
// expansion stops) then.
static bool WithinLimits(Mono* m, size_t depth, const char* name, const char* base, SourceRange range)
{
    if (depth <= MONO_MAX_DEPTH && strlen(name) <= MONO_MAX_NAME)
    {
        return true;
    }

    DiagErrorFmt(m->diag, range, "generic instantiation of '%s' is too deep (recursive generic type?)", base);
    m->stopped = true;

    return false;
}

// The nesting depth a struct instance was made at (0 for a name that is not an instance). A use that is
// already a leaf keeps the depth of the instance it names, so nesting is counted across expansions.
static size_t InstanceDepth(const Mono* m, const char* name)
{
    return (size_t)(uintptr_t)StrMapGet(&m->structDepths, name);
}

// -- Templates

// Moves every generic struct out of Module::structs. A second template of one name is a redefinition.
static void CollectStructTemplates(Mono* m)
{
    Module* mod = m->mod;
    size_t kept = 0;

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (strcmp(sd->name, "type") == 0)
        {
            DiagErrorFmt(m->diag, sd->base.range, "'type' is reserved and cannot name a struct");
        }

        if (sd->genericParams.count == 0)
        {
            VecSet(&mod->structs, kept++, sd);
            continue;
        }

        if (StrMapGet(&m->structTemplates, sd->name))
        {
            DiagErrorFmt(m->diag, sd->base.range, "redefinition of struct '%s'", sd->name);
        }
        else
        {
            StrMapPut(&m->structTemplates, sd->name, sd);
        }

        VecPush(&mod->genericTemplates, sd);
    }

    mod->structs.count = kept;
}

// Moves every impl of a generic struct out of Module::impls. Its methods (listed in Module::functions too) are
// added to `templateMethods` so the function pass drops them there; each is disposed through genericTemplates.
static void CollectImplTemplates(Mono* m, Vec* templateMethods)
{
    Module* mod = m->mod;
    size_t kept = 0;

    for (size_t i = 0; i < mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, i);
        const StructDecl* target = (const StructDecl*)StrMapGet(&m->structTemplates, impl->handleName);

        if (impl->genericParams.count == 0)
        {
            if (target)
            {
                DiagErrorFmt(m->diag, impl->base.range,
                             "generic struct '%s' needs an impl with its type parameters: 'impl %s<T>'",
                             impl->handleName, impl->handleName);
            }

            VecSet(&mod->impls, kept++, impl);
            continue;
        }

        if (!target)
        {
            DiagErrorFmt(m->diag, impl->base.range,
                         "'%s' is not a generic struct, so 'impl %s<...>' cannot bind type parameters",
                         impl->handleName, impl->handleName);
            VecSet(&mod->impls, kept++, impl);
            continue;
        }

        if (target->genericParams.count != impl->genericParams.count)
        {
            DiagErrorFmt(m->diag, impl->base.range, "impl '%s' binds %zu type parameter(s), but struct '%s' has %zu",
                         impl->handleName, impl->genericParams.count, impl->handleName, target->genericParams.count);
            VecSet(&mod->impls, kept++, impl);
            continue;
        }

        for (size_t j = 0; j < impl->properties.count; j++)
        {
            const PropertyDecl* prop = (const PropertyDecl*)VecGet(&impl->properties, j);
            DiagErrorFmt(m->diag, prop->range, "property '%s' in a generic impl is not supported", prop->name);
        }

        for (size_t j = 0; j < impl->methods.count; j++)
        {
            FunctionDecl* method = (FunctionDecl*)VecGet(&impl->methods, j);

            if (method->isExtern)
            {
                DiagErrorFmt(m->diag, method->base.range, "extern method '%s' in a generic impl is not supported",
                             method->methodName);
            }

            VecPush(templateMethods, method);
            VecPush(&mod->genericTemplates, method);
        }

        VecPush(&m->implTemplates, impl);
        VecPush(&mod->genericTemplates, impl);
    }

    mod->impls.count = kept;
}

// Moves every generic function out of Module::functions, and drops the impl methods already moved.
static void CollectFunctionTemplates(Mono* m, const Vec* templateMethods)
{
    Module* mod = m->mod;
    size_t kept = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, i);

        if (VecHas(templateMethods, fn))
        {
            continue;
        }

        if (fn->genericParams.count == 0)
        {
            VecSet(&mod->functions, kept++, fn);
            continue;
        }

        if (StrMapGet(&m->functionTemplates, fn->name))
        {
            DiagErrorFmt(m->diag, fn->base.range,
                         "generic function '%s' cannot be overloaded: give each generic function its own name",
                         fn->name);
        }
        else if (StrMapGet(&m->structTemplates, fn->name) || PlainStructNamed(mod, fn->name))
        {
            DiagErrorFmt(m->diag, fn->base.range,
                         "function '%s' has the name of a struct: 'Name(...)' constructs the struct, so the function "
                         "could never be called; rename one of them",
                         fn->name);
        }
        else
        {
            StrMapPut(&m->functionTemplates, fn->name, fn);
        }

        VecPush(&mod->genericTemplates, fn);
    }

    mod->functions.count = kept;
}

// The plain declarations left over must not share a name with a generic function or struct.
static void CheckPlainNames(Mono* m)
{
    Module* mod = m->mod;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);

        if (fn->fromImpl)
        {
            continue;
        }

        if (StrMapGet(&m->functionTemplates, fn->name))
        {
            DiagErrorFmt(m->diag, fn->base.range,
                         "generic function '%s' cannot be overloaded: give each generic function its own name",
                         fn->name);
        }
        else if (StrMapGet(&m->structTemplates, fn->name))
        {
            DiagErrorFmt(m->diag, fn->base.range, "function '%s' has the name of a generic struct: rename one of them",
                         fn->name);
        }
    }

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        const StructDecl* sd = (const StructDecl*)VecGet(&mod->structs, i);

        if (StrMapGet(&m->structTemplates, sd->name))
        {
            DiagErrorFmt(m->diag, sd->base.range, "redefinition of struct '%s'", sd->name);
        }
    }

    for (size_t i = 0; i < mod->enums.count; i++)
    {
        const EnumDecl* ed = (const EnumDecl*)VecGet(&mod->enums, i);

        if (strcmp(ed->name, "type") == 0)
        {
            DiagErrorFmt(m->diag, ed->base.range, "'type' is reserved and cannot name an enum");
        }
    }

    for (size_t i = 0; i < mod->handles.count; i++)
    {
        const HandleDecl* hd = (const HandleDecl*)VecGet(&mod->handles, i);

        if (strcmp(hd->name, "type") == 0)
        {
            DiagErrorFmt(m->diag, hd->base.range, "'type' is reserved and cannot name a handle");
        }
    }
}

// -- Templates

// Whether `t` uses the name `type` anywhere inside it.
static bool MentionsTypeKeyword(const TypeName* t)
{
    if (!t)
    {
        return false;
    }

    if (t->name && strcmp(t->name, "type") == 0 && !t->inner && !t->elem && t->typeArgCount == 0)
    {
        return true;
    }

    if (MentionsTypeKeyword(t->inner) || MentionsTypeKeyword(t->elem))
    {
        return true;
    }

    for (size_t i = 0; i < t->typeArgCount; i++)
    {
        if (MentionsTypeKeyword(&t->typeArgs[i]))
        {
            return true;
        }
    }

    return false;
}

// `type` is for the parameters of meta functions only. Templates are never rewritten, so their declarations are checked
// here; an error stops the expansion, so an instance does not report the same use again.
static void CheckTemplateTypeKeyword(Mono* m)
{
    static const char* const message = "'type' is only allowed for parameters of @meta functions";

    for (size_t i = 0; i < m->mod->genericTemplates.count; i++)
    {
        Node* n = (Node*)VecGet(&m->mod->genericTemplates, i);

        if (n->kind == NodeFunction)
        {
            const FunctionDecl* fn = (const FunctionDecl*)n;

            for (size_t j = 0; j < fn->params.count; j++)
            {
                const ParamDecl* param = (const ParamDecl*)VecGet(&fn->params, j);

                if (MentionsTypeKeyword(&param->type))
                {
                    DiagErrorFmt(m->diag, param->base.range, "%s", message);
                    m->stopped = true;
                }
            }

            if (MentionsTypeKeyword(&fn->returnType))
            {
                DiagErrorFmt(m->diag, fn->base.range, "%s", message);
                m->stopped = true;
            }
        }
        else if (n->kind == NodeStruct)
        {
            const StructDecl* sd = (const StructDecl*)n;

            for (size_t j = 0; j < sd->fields.count; j++)
            {
                if (MentionsTypeKeyword(&((const FieldDecl*)VecGet(&sd->fields, j))->type))
                {
                    DiagErrorFmt(m->diag, sd->base.range, "%s", message);
                    m->stopped = true;
                }
            }
        }
    }
}

// -- Instances

// Makes the struct `name` (the template `tmpl` at `args`), and every impl of the template at the same
// arguments. Its methods become functions named `<name>_<method>`.
static void InstantiateStruct(Mono* m, const StructDecl* tmpl, const char* name, const TypeName* args, size_t depth)
{
    if (StrMapGet(&m->structInstances, name) || !TakeInstanceSlot(m, tmpl->name, tmpl->base.range))
    {
        return;
    }

    TypeSubst subst = {&tmpl->genericParams, args};
    StructDecl* inst = (StructDecl*)AstCloneSubst(m->arena, (const Node*)tmpl, &subst);
    inst->name = (char*)name;
    FreeVec(&inst->genericParams);

    StrMapPut(&m->structInstances, name, inst);
    StrMapPut(&m->structDepths, name, (void*)(uintptr_t)depth);
    VecPush(&m->mod->structs, inst);
    VecPush(&m->pending, inst);

    for (size_t i = 0; i < m->implTemplates.count; i++)
    {
        const ImplDecl* tImpl = (const ImplDecl*)VecGet(&m->implTemplates, i);

        if (strcmp(tImpl->handleName, tmpl->name) != 0)
        {
            continue;
        }

        // Each impl binds its own parameter names, which are matched to the same arguments.
        TypeSubst implSubst = {&tImpl->genericParams, args};
        ImplDecl* impl = (ImplDecl*)AstCloneSubst(m->arena, (const Node*)tImpl, &implSubst);
        impl->handleName = (char*)name;
        FreeVec(&impl->genericParams);

        for (size_t j = 0; j < impl->methods.count; j++)
        {
            FunctionDecl* method = (FunctionDecl*)VecGet(&impl->methods, j);
            method->name = ArenaFormat(m->arena, "%s_%s", name, method->methodName);
            method->mangledName = method->name;

            VecPush(&m->mod->functions, method);
            VecPush(&m->pending, method);
        }

        VecPush(&m->mod->impls, impl);
        VecPush(&m->pending, impl);
    }
}

// Makes the function `name`: the template `tmpl` with its type parameters replaced by `args`.
static void InstantiateFunction(Mono* m, const FunctionDecl* tmpl, const char* name, const TypeName* args)
{
    if (StrMapGet(&m->functionInstances, name) || !TakeInstanceSlot(m, tmpl->name, tmpl->base.range))
    {
        return;
    }

    TypeSubst subst = {&tmpl->genericParams, args};
    FunctionDecl* inst = (FunctionDecl*)AstCloneSubst(m->arena, (const Node*)tmpl, &subst);
    inst->name = (char*)name;
    inst->mangledName = (char*)name;
    FreeVec(&inst->genericParams);

    StrMapPut(&m->functionInstances, name, inst);
    VecPush(&m->mod->functions, inst);
    VecPush(&m->pending, inst);
}

// -- Meta functions

// A `@meta` function as collected. An invalid one was reported when it was collected, and is never called.
typedef struct
{
    FunctionDecl* fn;
    bool valid;
} MetaFunc;

// A meta function that calls itself deeper than this stops with an error.
#define META_MAX_DEPTH 64

// The value of a meta expression: a 64-bit integer, or a bool (0 or 1 with isBool).
typedef struct
{
    int64_t value;
    bool isBool;
} MetaValue;

// A name bound while a meta function runs: a `type` parameter (with its type argument), or a bool or integer
// parameter or local (with its declared type and current value).
typedef struct
{
    const char* name;
    const char* typeName;
    bool isType;
    TypeName type;
    PrimitiveType prim;
    MetaValue value;
} MetaBinding;

// One meta call being interpreted. `fn` is NULL for a frame that runs no body: the top level, where a call written
// in code is folded, and the probes that test whether a condition is constant. A quiet frame reports nothing.
typedef struct
{
    Mono* m;
    const FunctionDecl* fn;
    Vec bindings;      // MetaBinding*: the parameters, then the locals of the open blocks
    size_t scopeStart; // the first binding of the innermost block
    unsigned depth;
    bool quiet;
    bool returned;
    MetaValue result;
} MetaFrame;

// Reports an error in the frame and yields false, unless the frame is quiet.
#define META_FAIL(frame, range, ...)                                                                                   \
    ((frame)->quiet ? false : (DiagErrorFmt((frame)->m->diag, (range), __VA_ARGS__), false))

static bool IsIntegerPrim(PrimitiveType p)
{
    switch (p)
    {
    case PrimInt:
    case PrimUInt:
    case PrimLong:
    case PrimULong:
    case PrimSByte:
    case PrimByte:
    case PrimShort:
    case PrimUShort:
        return true;
    default:
        return false;
    }
}

static bool IsUnsignedPrim(PrimitiveType p)
{
    return p == PrimUInt || p == PrimULong || p == PrimByte || p == PrimUShort;
}

// A plain name with no wrapper, no array shape and no type arguments: `int`, `Foo`.
static bool IsLeafType(const TypeName* t)
{
    return TypeNameIsBare(t) && !TypeNameIsArray(t) && t->typeArgCount == 0;
}

// The `type` of a meta parameter: the contextual name, as a leaf.
static bool IsTypeParamType(const TypeName* t)
{
    return IsLeafType(t) && strcmp(t->name, "type") == 0;
}

// The types a meta parameter, local or result may have: bool and the integer types.
static bool IsScalarMetaType(const TypeName* t)
{
    return IsLeafType(t) && (t->primitiveType == PrimBool || IsIntegerPrim(t->primitiveType));
}

// Whether `v` fits the bool or integer type `prim`. A ulong is held in 64 signed bits, so it stops at INT64_MAX.
static bool MetaFits(PrimitiveType prim, int64_t v)
{
    switch (prim)
    {
    case PrimByte:
        return v >= 0 && v <= UINT8_MAX;
    case PrimSByte:
        return v >= INT8_MIN && v <= INT8_MAX;
    case PrimShort:
        return v >= INT16_MIN && v <= INT16_MAX;
    case PrimUShort:
        return v >= 0 && v <= UINT16_MAX;
    case PrimInt:
        return v >= INT32_MIN && v <= INT32_MAX;
    case PrimUInt:
        return v >= 0 && v <= UINT32_MAX;
    case PrimULong:
        return v >= 0;
    default:
        return true;
    }
}

static void MetaFrameInit(MetaFrame* f, Mono* m, const FunctionDecl* fn, bool quiet)
{
    f->m = m;
    f->fn = fn;
    VecInit(&f->bindings);
    f->scopeStart = 0;
    f->depth = 0;
    f->quiet = quiet;
    f->returned = false;
    f->result = (MetaValue){0, false};
}

static MetaBinding* MetaBindNew(MetaFrame* f, const char* name, const char* typeName)
{
    MetaBinding* b = AST_NEW(f->m->arena, MetaBinding);
    b->name = name;
    b->typeName = typeName;
    b->isType = false;
    b->type = (TypeName){0};
    b->prim = PrimNone;
    b->value = (MetaValue){0, false};
    VecPush(&f->bindings, b);

    return b;
}

static MetaBinding* MetaLookup(const MetaFrame* f, const char* name)
{
    for (size_t i = f->bindings.count; i > 0; i--)
    {
        MetaBinding* b = (MetaBinding*)VecGet(&f->bindings, i - 1);

        if (strcmp(b->name, name) == 0)
        {
            return b;
        }
    }

    return NULL;
}

// Stores `in` into a slot of the type `typeName` (a bool, or the integer type `prim`), checking its kind and range.
static bool MetaStore(MetaFrame* f, SourceRange range, PrimitiveType prim, const char* typeName, MetaValue in,
                      MetaValue* out)
{
    if (in.isBool != (prim == PrimBool))
    {
        return META_FAIL(f, range, "'%s' needs a %s value", typeName, prim == PrimBool ? "bool" : "integer");
    }

    if (!MetaFits(prim, in.value))
    {
        return META_FAIL(f, range, "value %lld is out of range for '%s'", (long long)in.value, typeName);
    }

    *out = in;

    return true;
}

// The type an argument stands for, with the type parameters bound in `f` replaced by their arguments.
static TypeName MetaResolveType(MetaFrame* f, const TypeName* t)
{
    size_t count = 0;

    for (size_t i = 0; i < f->bindings.count; i++)
    {
        count += ((MetaBinding*)VecGet(&f->bindings, i))->isType ? 1 : 0;
    }

    Vec names;
    VecInit(&names);
    TypeName* args = (TypeName*)ArenaAlloc(f->m->arena, (count ? count : 1) * sizeof(TypeName));
    size_t k = 0;

    for (size_t i = 0; i < f->bindings.count; i++)
    {
        const MetaBinding* b = (const MetaBinding*)VecGet(&f->bindings, i);

        if (b->isType)
        {
            VecPush(&names, (void*)b->name);
            args[k++] = b->type;
        }
    }

    TypeSubst subst = {&names, args};
    TypeName result = TypeNameSubstitute(f->m->arena, t, &subst);
    free(names.items);

    return result;
}

// Whether the impl `impl` has a method whose `methodName` is `method`.
static bool ImplHasMethodNamed(const ImplDecl* impl, const char* method)
{
    for (size_t i = 0; i < impl->methods.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&impl->methods, i);

        if (fn->methodName && strcmp(fn->methodName, method) == 0)
        {
            return true;
        }
    }

    return false;
}

// Whether the struct or handle `t` has an impl method named `method`. The impl templates of a generic struct are
// searched by its base name, since an instance may not have been made yet.
static bool MetaTypeHasMethod(const Mono* m, const TypeName* t, const char* method)
{
    // Wrapped types and array shapes have no impl of their own.
    if (!TypeNameIsBare(t) || TypeNameIsArray(t))
    {
        return false;
    }

    const char* base = t->genericBase ? t->genericBase : t->name;

    if (StrMapGet(&m->structTemplates, base))
    {
        for (size_t i = 0; i < m->implTemplates.count; i++)
        {
            const ImplDecl* impl = (const ImplDecl*)VecGet(&m->implTemplates, i);

            if (strcmp(impl->handleName, base) == 0 && ImplHasMethodNamed(impl, method))
            {
                return true;
            }
        }

        return false;
    }

    for (size_t i = 0; i < m->mod->impls.count; i++)
    {
        const ImplDecl* impl = (const ImplDecl*)VecGet(&m->mod->impls, i);

        if (strcmp(impl->handleName, base) == 0 && ImplHasMethodNamed(impl, method))
        {
            return true;
        }
    }

    return false;
}

static bool MetaHasFunction(MetaFrame* f, const MetaCallExpr* call, MetaValue* out)
{
    if (call->args.count != 2)
    {
        return META_FAIL(f, call->base.range, "'@has_function' takes 2 arguments, but %zu were given",
                         call->args.count);
    }

    const MetaArg* subject = (const MetaArg*)VecGet(&call->args, 0);
    const MetaArg* method = (const MetaArg*)VecGet(&call->args, 1);

    if (!subject->type)
    {
        return META_FAIL(f, call->base.range, "'@has_function' expects a type as its first argument");
    }

    if (!method->type || !IsLeafType(method->type))
    {
        return META_FAIL(f, call->base.range, "'@has_function' expects a method name as its second argument");
    }

    TypeName type = MetaResolveType(f, subject->type);
    out->value = MetaTypeHasMethod(f->m, &type, method->type->name);
    out->isBool = true;

    return true;
}

static bool MetaIsSame(MetaFrame* f, const MetaCallExpr* call, MetaValue* out)
{
    if (call->args.count != 2)
    {
        return META_FAIL(f, call->base.range, "'@is_same' takes 2 arguments, but %zu were given", call->args.count);
    }

    const MetaArg* a = (const MetaArg*)VecGet(&call->args, 0);
    const MetaArg* b = (const MetaArg*)VecGet(&call->args, 1);

    if (!a->type || !b->type)
    {
        return META_FAIL(f, call->base.range, "'@is_same' expects types as its arguments");
    }

    // The canonical names carry no `const`, and a strong alias has a name of its own.
    TypeName left = MetaResolveType(f, a->type);
    TypeName right = MetaResolveType(f, b->type);
    out->value = strcmp(left.name, right.name) == 0;
    out->isBool = true;

    return true;
}

static bool MetaEvalCall(MetaFrame* caller, const MetaCallExpr* call, MetaValue* out, PrimitiveType* outPrim);

static bool MetaEvalExpr(MetaFrame* f, const Node* n, MetaValue* out);

static bool MetaExecStmt(MetaFrame* f, const Node* n);

// Binds each argument of `call` to its parameter in `callee`. A `type` parameter takes a type argument; a bool or
// integer parameter takes a value, read from the caller's expression or from a name the caller binds.
static bool MetaBindArgs(MetaFrame* caller, MetaFrame* callee, const MetaCallExpr* call)
{
    const FunctionDecl* fn = callee->fn;

    for (size_t i = 0; i < fn->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&fn->params, i);
        const MetaArg* arg = (const MetaArg*)VecGet(&call->args, i);
        MetaBinding* b = MetaBindNew(callee, param->name, param->type.name);

        if (IsTypeParamType(&param->type))
        {
            if (!arg->type)
            {
                return META_FAIL(caller, call->base.range, "meta function '@%s' expects a type for parameter '%s'",
                                 fn->name, param->name);
            }

            MetaBinding* named = IsLeafType(arg->type) ? MetaLookup(caller, arg->type->name) : NULL;

            if (named && !named->isType)
            {
                return META_FAIL(caller, call->base.range, "'%s' is a value, but '@%s' expects a type for '%s'",
                                 arg->type->name, fn->name, param->name);
            }

            b->isType = true;
            b->type = MetaResolveType(caller, arg->type);
            continue;
        }

        MetaValue value = {0, false};

        if (arg->expr)
        {
            if (!MetaEvalExpr(caller, arg->expr, &value))
            {
                return false;
            }
        }
        else
        {
            MetaBinding* named = IsLeafType(arg->type) ? MetaLookup(caller, arg->type->name) : NULL;

            if (!named || named->isType)
            {
                return META_FAIL(caller, call->base.range,
                                 "meta function '@%s' expects a value for parameter '%s', but '%s' is not a value",
                                 fn->name, param->name, arg->type->name);
            }

            value = named->value;
        }

        if (!MetaStore(callee, call->base.range, param->type.primitiveType, param->type.name, value, &b->value))
        {
            return false;
        }

        b->prim = param->type.primitiveType;
    }

    return true;
}

// Runs the body of the meta function `call` names, in a frame of its own. `outPrim` is its result type.
static bool MetaEvalCall(MetaFrame* caller, const MetaCallExpr* call, MetaValue* out, PrimitiveType* outPrim)
{
    if (strcmp(call->name, "has_function") == 0)
    {
        *outPrim = PrimBool;

        return MetaHasFunction(caller, call, out);
    }

    if (strcmp(call->name, "is_same") == 0)
    {
        *outPrim = PrimBool;

        return MetaIsSame(caller, call, out);
    }

    const MetaFunc* mf = (const MetaFunc*)StrMapGet(&caller->m->metaFunctions, call->name);

    if (!mf)
    {
        return META_FAIL(caller, call->base.range, "unknown meta function '@%s'", call->name);
    }

    if (!mf->valid)
    {
        return false;
    }

    const FunctionDecl* fn = mf->fn;
    *outPrim = fn->returnType.primitiveType;

    if (call->args.count != fn->params.count)
    {
        return META_FAIL(caller, call->base.range, "meta function '@%s' takes %zu argument(s), but %zu were given",
                         call->name, fn->params.count, call->args.count);
    }

    MetaFrame callee;
    MetaFrameInit(&callee, caller->m, fn, caller->quiet);
    callee.depth = caller->depth + 1;

    bool ok = true;

    if (callee.depth > META_MAX_DEPTH)
    {
        ok = META_FAIL(caller, call->base.range, "meta function '@%s' recurses too deeply", call->name);
    }

    ok = ok && MetaBindArgs(caller, &callee, call);
    ok = ok && MetaExecStmt(&callee, fn->body);

    if (ok && !callee.returned)
    {
        ok = META_FAIL(caller, call->base.range, "meta function '@%s' reaches its end without returning", call->name);
    }

    if (ok)
    {
        *out = callee.result;
    }

    free(callee.bindings.items);

    return ok;
}

// Applies a unary operator to a meta value.
static bool MetaUnary(MetaFrame* f, SourceRange range, UnaryOp op, MetaValue v, MetaValue* out)
{
    switch (op)
    {
    case UnNot:
        if (!v.isBool)
        {
            return META_FAIL(f, range, "operand of '!' must be bool");
        }

        out->value = !v.value;
        out->isBool = true;
        return true;
    case UnNeg:
    case UnPos:
    case UnBitNot:
        if (v.isBool)
        {
            return META_FAIL(f, range, "operand of this operator must be an integer");
        }

        out->value = op == UnNeg ? (int64_t)(0 - (uint64_t)v.value) : (op == UnBitNot ? ~v.value : v.value);
        out->isBool = false;
        return true;
    default:
        return META_FAIL(f, range, "this operator can't be evaluated at compile time");
    }
}

// Applies a binary operator to two meta values, except `&&` and `||`, which the caller stops early. Integer
// arithmetic wraps at 64 bits.
static bool MetaBinary(MetaFrame* f, SourceRange range, BinaryOp op, MetaValue a, MetaValue b, MetaValue* out)
{
    if (op == BinEqEq || op == BinNotEq)
    {
        if (a.isBool != b.isBool)
        {
            return META_FAIL(f, range, "operands of '==' and '!=' must both be bool or both be integers");
        }

        out->value = (a.value == b.value) == (op == BinEqEq);
        out->isBool = true;
        return true;
    }

    if (a.isBool && b.isBool && (op == BinBitAnd || op == BinBitOr || op == BinBitXor))
    {
        out->value
            = op == BinBitAnd ? (a.value & b.value) : (op == BinBitOr ? (a.value | b.value) : (a.value ^ b.value));
        out->isBool = true;
        return true;
    }

    if (a.isBool || b.isBool)
    {
        return META_FAIL(f, range, "operands of this operator must be integers");
    }

    uint64_t ua = (uint64_t)a.value;
    uint64_t ub = (uint64_t)b.value;
    out->isBool = false;

    switch (op)
    {
    case BinAdd:
        out->value = (int64_t)(ua + ub);
        return true;
    case BinSub:
        out->value = (int64_t)(ua - ub);
        return true;
    case BinMul:
        out->value = (int64_t)(ua * ub);
        return true;
    case BinDiv:
    case BinMod:
        if (b.value == 0)
        {
            return META_FAIL(f, range, "division by zero in a meta function");
        }

        // INT64_MIN / -1 does not fit in 64 bits; it wraps, and its remainder is 0.
        if (b.value == -1)
        {
            out->value = op == BinDiv ? (int64_t)(0 - ua) : 0;
        }
        else
        {
            out->value = op == BinDiv ? a.value / b.value : a.value % b.value;
        }
        return true;
    case BinBitAnd:
        out->value = a.value & b.value;
        return true;
    case BinBitOr:
        out->value = a.value | b.value;
        return true;
    case BinBitXor:
        out->value = a.value ^ b.value;
        return true;
    case BinShl:
    case BinShr:
        if (b.value < 0 || b.value > 63)
        {
            return META_FAIL(f, range, "shift count %lld is out of range", (long long)b.value);
        }

        out->value = op == BinShl ? (int64_t)(ua << b.value) : (a.value >> b.value);
        return true;
    case BinLt:
        out->value = a.value < b.value;
        out->isBool = true;
        return true;
    case BinLtEq:
        out->value = a.value <= b.value;
        out->isBool = true;
        return true;
    case BinGt:
        out->value = a.value > b.value;
        out->isBool = true;
        return true;
    case BinGtEq:
        out->value = a.value >= b.value;
        out->isBool = true;
        return true;
    default:
        return META_FAIL(f, range, "this operator can't be evaluated at compile time");
    }
}

// A name read as a value: a bool or integer parameter or local.
static bool MetaEvalIdent(MetaFrame* f, const IdentExpr* e, MetaValue* out)
{
    const MetaBinding* b = MetaLookup(f, e->name);

    if (!b)
    {
        if (f->fn)
        {
            return META_FAIL(f, e->base.range, "'%s' is not a parameter or local of meta function '@%s'", e->name,
                             f->fn->name);
        }

        return META_FAIL(f, e->base.range, "'%s' is not a value known at compile time", e->name);
    }

    if (b->isType)
    {
        return META_FAIL(f, e->base.range, "'%s' is a type, not a value", e->name);
    }

    *out = b->value;

    return true;
}

// A bool expression, such as an `if` condition.
static bool MetaEvalBool(MetaFrame* f, const Node* n, bool* out)
{
    MetaValue v;

    if (!MetaEvalExpr(f, n, &v))
    {
        return false;
    }

    if (!v.isBool)
    {
        return META_FAIL(f, n->range, "a condition must be bool");
    }

    *out = v.value != 0;

    return true;
}

static bool MetaEvalBinary(MetaFrame* f, const BinaryExpr* e, MetaValue* out)
{
    MetaValue lhs;

    if (!MetaEvalExpr(f, e->lhs, &lhs))
    {
        return false;
    }

    if (e->op == BinLogicAnd || e->op == BinLogicOr)
    {
        if (!lhs.isBool)
        {
            return META_FAIL(f, e->base.range, "operands of '&&' and '||' must be bool");
        }

        // `&&` is decided by a false left side, `||` by a true one; the right side is then not evaluated.
        bool decidedBy = e->op == BinLogicOr;

        if ((lhs.value != 0) == decidedBy)
        {
            out->value = decidedBy;
            out->isBool = true;
            return true;
        }

        MetaValue rhs;

        if (!MetaEvalExpr(f, e->rhs, &rhs))
        {
            return false;
        }

        if (!rhs.isBool)
        {
            return META_FAIL(f, e->base.range, "operands of '&&' and '||' must be bool");
        }

        out->value = rhs.value;
        out->isBool = true;
        return true;
    }

    MetaValue rhs;

    if (!MetaEvalExpr(f, e->rhs, &rhs))
    {
        return false;
    }

    return MetaBinary(f, e->base.range, e->op, lhs, rhs, out);
}

// `local = value` and `local op= value`: the target is a bool or integer parameter or local.
static bool MetaAssign(MetaFrame* f, const AssignExpr* e, MetaValue* out)
{
    if (e->target->kind != NodeIdent)
    {
        return META_FAIL(f, e->base.range, "only a parameter or local can be assigned in a meta function");
    }

    const IdentExpr* target = (const IdentExpr*)e->target;
    MetaBinding* b = MetaLookup(f, target->name);

    if (!b || b->isType)
    {
        if (f->fn)
        {
            return META_FAIL(f, e->base.range, "'%s' is not a parameter or local of meta function '@%s'", target->name,
                             f->fn->name);
        }

        return META_FAIL(f, e->base.range, "'%s' is not a value known at compile time", target->name);
    }

    MetaValue rhs;

    if (!MetaEvalExpr(f, e->value, &rhs))
    {
        return false;
    }

    MetaValue next = rhs;

    if (e->op != AssignSet)
    {
        BinaryOp op = e->op == AssignAdd   ? BinAdd
                      : e->op == AssignSub ? BinSub
                      : e->op == AssignMul ? BinMul
                      : e->op == AssignDiv ? BinDiv
                                           : BinMod;

        if (!MetaBinary(f, e->base.range, op, b->value, rhs, &next))
        {
            return false;
        }
    }

    if (!MetaStore(f, e->base.range, b->prim, b->typeName, next, &b->value))
    {
        return false;
    }

    *out = b->value;

    return true;
}

// The value of an expression in a meta function body, or of a constant expression in code.
static bool MetaEvalExpr(MetaFrame* f, const Node* n, MetaValue* out)
{
    switch (n->kind)
    {
    case NodeIntLiteral:
        out->value = (int64_t)((const IntLiteral*)n)->value;
        out->isBool = false;
        return true;
    case NodeBoolLiteral:
        out->value = ((const BoolLiteral*)n)->value ? 1 : 0;
        out->isBool = true;
        return true;
    case NodeIdent:
        return MetaEvalIdent(f, (const IdentExpr*)n, out);
    case NodeUnary:
    {
        const UnaryExpr* e = (const UnaryExpr*)n;
        MetaValue operand;

        return MetaEvalExpr(f, e->operand, &operand) && MetaUnary(f, n->range, e->op, operand, out);
    }
    case NodeBinary:
        return MetaEvalBinary(f, (const BinaryExpr*)n, out);
    case NodeConditional:
    {
        const ConditionalExpr* e = (const ConditionalExpr*)n;
        bool cond = false;

        if (!MetaEvalBool(f, e->condition, &cond))
        {
            return false;
        }

        return MetaEvalExpr(f, cond ? e->thenExpr : e->elseExpr, out);
    }
    case NodeAssign:
        return MetaAssign(f, (const AssignExpr*)n, out);
    case NodeMetaCall:
    {
        PrimitiveType prim = PrimNone;

        return MetaEvalCall(f, (const MetaCallExpr*)n, out, &prim);
    }
    default:
        return META_FAIL(f, n->range, "this expression can't be evaluated at compile time");
    }
}

// A `bool x = init;` or `int x = init;` local, bound in the innermost block.
static bool MetaExecVarDecl(MetaFrame* f, const VarDeclStmt* v)
{
    for (size_t i = f->scopeStart; i < f->bindings.count; i++)
    {
        if (strcmp(((const MetaBinding*)VecGet(&f->bindings, i))->name, v->name) == 0)
        {
            return META_FAIL(f, v->base.range, "'%s' is already declared in this block", v->name);
        }
    }

    if (!v->init)
    {
        return META_FAIL(f, v->base.range, "local '%s' needs an initializer", v->name);
    }

    MetaValue init = {0, false};

    if (!MetaEvalExpr(f, v->init, &init))
    {
        return false;
    }

    MetaBinding* b = MetaBindNew(f, v->name, v->type.name);
    b->prim = v->type.primitiveType;

    return MetaStore(f, v->base.range, b->prim, b->typeName, init, &b->value);
}

// A block opens a scope: its locals are dropped when it ends.
static bool MetaExecBlock(MetaFrame* f, const Block* block)
{
    size_t bindingsBefore = f->bindings.count;
    size_t scopeBefore = f->scopeStart;
    bool ok = true;

    f->scopeStart = bindingsBefore;

    for (size_t i = 0; ok && !f->returned && i < block->statements.count; i++)
    {
        ok = MetaExecStmt(f, (const Node*)VecGet(&block->statements, i));
    }

    f->bindings.count = bindingsBefore;
    f->scopeStart = scopeBefore;

    return ok;
}

static bool MetaExecStmt(MetaFrame* f, const Node* n)
{
    switch (n->kind)
    {
    case NodeBlock:
        return MetaExecBlock(f, (const Block*)n);
    case NodeReturn:
    {
        const ReturnStmt* r = (const ReturnStmt*)n;
        MetaValue v = {0, false};

        if (!r->value)
        {
            return META_FAIL(f, n->range, "'return' needs a value in meta function '@%s'", f->fn->name);
        }

        if (!MetaEvalExpr(f, r->value, &v))
        {
            return false;
        }

        f->returned = true;

        return MetaStore(f, n->range, f->fn->returnType.primitiveType, f->fn->returnType.name, v, &f->result);
    }
    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        bool cond = false;

        if (!MetaEvalBool(f, s->condition, &cond))
        {
            return false;
        }

        if (cond)
        {
            return MetaExecStmt(f, s->thenBranch);
        }

        return s->elseBranch ? MetaExecStmt(f, s->elseBranch) : true;
    }
    case NodeVarDecl:
        return MetaExecVarDecl(f, (const VarDeclStmt*)n);
    case NodeExprStmt:
    {
        const Node* expr = ((const ExprStmt*)n)->expr;
        MetaValue discard = {0, false};

        return expr ? MetaEvalExpr(f, expr, &discard) : true;
    }
    default:
        return META_FAIL(f, n->range, "this statement can't be evaluated at compile time");
    }
}

// -- Meta function bodies

// Reports a construct that a meta function body cannot use, and yields false.
static bool MetaConstructError(Mono* m, const FunctionDecl* fn, const Node* n, const char* what)
{
    DiagErrorFmt(m->diag, n->range, "%s can't be evaluated at compile time in meta function '@%s'", what, fn->name);

    return false;
}

// A meta function body is interpreted, so it may use only what MetaExecStmt and MetaEvalExpr handle. Everything else
// is reported here, when the function is collected, whether or not it is ever called.
static bool CheckMetaExpr(Mono* m, const FunctionDecl* fn, const Node* n)
{
    if (!n)
    {
        return true;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    case NodeBoolLiteral:
    case NodeIdent:
        return true;
    case NodeUnary:
    {
        const UnaryExpr* e = (const UnaryExpr*)n;

        if (e->op == UnAddrOf)
        {
            return MetaConstructError(m, fn, n, "'&'");
        }

        if (e->op == UnDeref)
        {
            return MetaConstructError(m, fn, n, "'*'");
        }

        return CheckMetaExpr(m, fn, e->operand);
    }
    case NodeBinary:
        return CheckMetaExpr(m, fn, ((const BinaryExpr*)n)->lhs) && CheckMetaExpr(m, fn, ((const BinaryExpr*)n)->rhs);
    case NodeAssign:
        return CheckMetaExpr(m, fn, ((const AssignExpr*)n)->target)
               && CheckMetaExpr(m, fn, ((const AssignExpr*)n)->value);
    case NodeConditional:
    {
        const ConditionalExpr* e = (const ConditionalExpr*)n;

        return CheckMetaExpr(m, fn, e->condition) && CheckMetaExpr(m, fn, e->thenExpr)
               && CheckMetaExpr(m, fn, e->elseExpr);
    }
    case NodeMetaCall:
    {
        const MetaCallExpr* call = (const MetaCallExpr*)n;

        for (size_t i = 0; i < call->args.count; i++)
        {
            if (!CheckMetaExpr(m, fn, ((const MetaArg*)VecGet(&call->args, i))->expr))
            {
                return false;
            }
        }

        return true;
    }
    case NodeCall:
        return MetaConstructError(m, fn, n, "a call");
    case NodeFloatLiteral:
        return MetaConstructError(m, fn, n, "a float literal");
    case NodeStrLiteral:
        return MetaConstructError(m, fn, n, "a string literal");
    case NodeMember:
        return MetaConstructError(m, fn, n, "a member access");
    case NodeIndex:
        return MetaConstructError(m, fn, n, "an index");
    case NodeStructInit:
        return MetaConstructError(m, fn, n, "a struct literal");
    case NodeArrayInit:
        return MetaConstructError(m, fn, n, "an array literal");
    case NodeCast:
        return MetaConstructError(m, fn, n, "a cast");
    case NodeNullTest:
        return MetaConstructError(m, fn, n, "a null test");
    case NodeIncDec:
        return MetaConstructError(m, fn, n, "'++' or '--'");
    default:
        return MetaConstructError(m, fn, n, "this expression");
    }
}

static bool CheckMetaStmt(Mono* m, const FunctionDecl* fn, const Node* n)
{
    if (!n)
    {
        return true;
    }

    switch (n->kind)
    {
    case NodeBlock:
    {
        const Block* block = (const Block*)n;

        for (size_t i = 0; i < block->statements.count; i++)
        {
            if (!CheckMetaStmt(m, fn, (const Node*)VecGet(&block->statements, i)))
            {
                return false;
            }
        }

        return true;
    }
    case NodeReturn:
        return CheckMetaExpr(m, fn, ((const ReturnStmt*)n)->value);
    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;

        return CheckMetaExpr(m, fn, s->condition) && CheckMetaStmt(m, fn, s->thenBranch)
               && CheckMetaStmt(m, fn, s->elseBranch);
    }
    case NodeVarDecl:
    {
        const VarDeclStmt* v = (const VarDeclStmt*)n;

        if (v->isRef)
        {
            return MetaConstructError(m, fn, n, "a 'ref' local");
        }

        if (v->isVar)
        {
            return MetaConstructError(m, fn, n, "a 'var' local");
        }

        if (IsTypeParamType(&v->type))
        {
            DiagErrorFmt(m->diag, n->range, "'type' is only allowed for parameters of @meta functions");

            return false;
        }

        if (!IsScalarMetaType(&v->type))
        {
            DiagErrorFmt(m->diag, n->range, "local '%s' in meta function '@%s' must be bool or an integer type",
                         v->name, fn->name);

            return false;
        }

        if (!v->init)
        {
            DiagErrorFmt(m->diag, n->range, "local '%s' in meta function '@%s' needs an initializer", v->name,
                         fn->name);

            return false;
        }

        return CheckMetaExpr(m, fn, v->init);
    }
    case NodeExprStmt:
        return CheckMetaExpr(m, fn, ((const ExprStmt*)n)->expr);
    case NodeWhile:
        return MetaConstructError(m, fn, n, "'while'");
    case NodeFor:
        return MetaConstructError(m, fn, n, "'for'");
    case NodeDefer:
        return MetaConstructError(m, fn, n, "'defer'");
    case NodeBreak:
        return MetaConstructError(m, fn, n, "'break'");
    case NodeContinue:
        return MetaConstructError(m, fn, n, "'continue'");
    default:
        return MetaConstructError(m, fn, n, "this statement");
    }
}

// Checks the signature and body of each meta function and registers it. A name used twice is a redefinition, and the
// built-in names are not available.
static void CollectMetaFunctions(Mono* m)
{
    Module* mod = m->mod;

    for (size_t i = 0; i < mod->metaFunctions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->metaFunctions, i);
        bool isBuiltin = strcmp(fn->name, "has_function") == 0 || strcmp(fn->name, "is_same") == 0;
        bool valid = fn->body != NULL;

        if (isBuiltin)
        {
            DiagErrorFmt(m->diag, fn->base.range, "'@%s' is a built-in meta function and cannot be redefined",
                         fn->name);
            valid = false;
        }
        else if (StrMapGet(&m->metaFunctions, fn->name))
        {
            DiagErrorFmt(m->diag, fn->base.range, "redefinition of meta function '@%s'", fn->name);
            valid = false;
        }

        for (size_t j = 0; j < fn->params.count; j++)
        {
            const ParamDecl* param = (const ParamDecl*)VecGet(&fn->params, j);

            if (ByRef(param->mod))
            {
                DiagErrorFmt(m->diag, param->base.range, "meta function '@%s' parameter '%s' cannot be 'ref'", fn->name,
                             param->name);
                valid = false;
            }
            else if (!IsTypeParamType(&param->type) && !IsScalarMetaType(&param->type))
            {
                DiagErrorFmt(m->diag, param->base.range,
                             "meta function '@%s' parameter '%s' must be 'type', bool or an integer type", fn->name,
                             param->name);
                valid = false;
            }
        }

        if (!IsScalarMetaType(&fn->returnType))
        {
            DiagErrorFmt(m->diag, fn->base.range, "meta function '@%s' must return bool or an integer type", fn->name);
            valid = false;
        }

        if (valid)
        {
            valid = CheckMetaStmt(m, fn, fn->body);
        }

        if (isBuiltin || StrMapGet(&m->metaFunctions, fn->name))
        {
            continue;
        }

        MetaFunc* mf = (MetaFunc*)ArenaAlloc(m->arena, sizeof(MetaFunc));
        mf->fn = fn;
        mf->valid = valid;
        StrMapPut(&m->metaFunctions, fn->name, mf);
    }
}

// -- Folding meta calls and constant conditions

// The literal a folded meta call becomes: a bool, or an integer with the signedness and width of its result type.
static Node* MetaLiteralNode(Mono* m, MetaValue v, PrimitiveType prim, SourceRange range)
{
    if (prim == PrimBool)
    {
        BoolLiteral* lit = AST_NEW(m->arena, BoolLiteral);
        lit->base.kind = NodeBoolLiteral;
        lit->base.range = range;
        lit->value = v.value != 0;

        return (Node*)lit;
    }

    IntLiteral* lit = AST_NEW(m->arena, IntLiteral);
    lit->base.kind = NodeIntLiteral;
    lit->base.range = range;
    lit->value = v.value < 0 ? 0 - (uint64_t)v.value : (uint64_t)v.value;
    lit->isUnsigned = IsUnsignedPrim(prim);
    lit->isLong = prim == PrimLong || prim == PrimULong;

    if (v.value >= 0)
    {
        return (Node*)lit;
    }

    // A negative value is a negated literal, as the parser reads `-5`.
    UnaryExpr* neg = AST_NEW(m->arena, UnaryExpr);
    neg->base.kind = NodeUnary;
    neg->base.range = range;
    neg->op = UnNeg;
    neg->operand = (Node*)lit;
    neg->pointee = NULL;

    return (Node*)neg;
}

// Folds a meta call written in code. A call that fails is reported, and folds to a placeholder (false, or 0 for an
// integer result) so the rest of the expansion still runs.
static Node* FoldMetaCall(Mono* m, const MetaCallExpr* call)
{
    MetaFrame root;
    MetaFrameInit(&root, m, NULL, false);

    MetaValue value = {0, false};
    PrimitiveType prim = PrimBool;

    if (!MetaEvalCall(&root, call, &value, &prim))
    {
        value = (MetaValue){0, prim == PrimBool};
    }

    free(root.bindings.items);
    m->metaCalls++;

    return MetaLiteralNode(m, value, prim, call->base.range);
}

// Whether the rewritten condition `cond` is a constant, and its value. Only literals and operators fold: a name or a
// call does not, and nothing is reported when one is found.
static bool FoldCondition(Mono* m, const Node* cond, bool* out)
{
    MetaFrame probe;
    MetaFrameInit(&probe, m, NULL, true);

    bool value = false;
    bool folded = MetaEvalBool(&probe, cond, &value);

    free(probe.bindings.items);
    *out = value;

    return folded;
}

// -- Rewriting uses

static void RewriteNode(Mono* m, Node* n);
static void RewriteSlot(Mono* m, Node** slot);

static void RewriteList(Mono* m, const Vec* list)
{
    for (size_t i = 0; i < list->count; i++)
    {
        RewriteNode(m, (Node*)VecGet(list, i));
    }
}

// Rewrites each statement or expression of a Vec in place: a slot may be replaced by a literal or by a branch.
static void RewriteSlotList(Mono* m, Vec* list)
{
    for (size_t i = 0; i < list->count; i++)
    {
        Node* item = (Node*)VecGet(list, i);

        RewriteSlot(m, &item);
        VecSet(list, i, item);
    }
}

// Rewrites the generic uses inside `t` into leaves, inner uses first. Returns the type-argument nesting depth
// of `t`: a use with type arguments is one level deeper than the deepest use inside its arguments.
static size_t RewriteType(Mono* m, TypeName* t)
{
    size_t depth = 0;

    if (t->inner)
    {
        depth = MaxSize(depth, RewriteType(m, t->inner));
    }

    if (t->elem)
    {
        depth = MaxSize(depth, RewriteType(m, t->elem));
    }

    for (size_t i = 0; i < t->typeArgCount; i++)
    {
        depth = MaxSize(depth, RewriteType(m, &t->typeArgs[i]));
    }

    if (t->typeArgCount == 0)
    {
        if (!t->inner && !t->elem && t->name)
        {
            depth = MaxSize(depth, InstanceDepth(m, t->name));

            if (StrMapGet(&m->structTemplates, t->name))
            {
                DiagErrorFmt(m->diag, t->range, "generic struct '%s' needs type arguments: write '%s<...>'", t->name,
                             t->name);
            }
            else if (strcmp(t->name, "type") == 0)
            {
                DiagErrorFmt(m->diag, t->range, "'type' is only allowed for parameters of @meta functions");
            }
        }

        return depth;
    }

    const StructDecl* tmpl = (const StructDecl*)StrMapGet(&m->structTemplates, t->genericBase);
    char* name = TypeNameGenericSpelling(m->arena, t->genericBase, t->typeArgs, t->typeArgCount);
    size_t useDepth = depth + 1;

    if (!tmpl)
    {
        DiagErrorFmt(m->diag, t->range, "'%s' is not a generic struct, so it takes no type arguments", t->genericBase);
    }
    else if (tmpl->genericParams.count != t->typeArgCount)
    {
        DiagErrorFmt(m->diag, t->range, "generic struct '%s' takes %zu type argument(s), but %zu were given",
                     t->genericBase, tmpl->genericParams.count, t->typeArgCount);
    }
    else if (WithinLimits(m, useDepth, name, t->genericBase, t->range))
    {
        InstantiateStruct(m, tmpl, name, t->typeArgs, useDepth);
    }

    // The use is now the plain name of its instance; the spelling is the same as the parser's.
    TypeName leaf = TypeNameLeaf(name);
    leaf.range = t->range;
    leaf.isConst = t->isConst;
    *t = leaf;

    return useDepth;
}

// `holder<int>(args)`: a constructor call on the instance `holder<int>`, which is named by its canonical spelling
// like any other use of the struct.
static void RewriteGenericStructCall(Mono* m, CallExpr* call)
{
    TypeName use = {0};
    use.name = call->callee;
    use.genericBase = call->callee;
    use.typeArgs = call->typeArgs;
    use.typeArgCount = call->typeArgCount;
    use.range = call->base.range;

    RewriteType(m, &use);

    call->callee = use.name;
    call->typeArg = NULL;
    call->typeArgs = NULL;
    call->typeArgCount = 0;
}

// A call to a generic function becomes a call to its instance, named by the canonical spelling. Other
// calls are left for sema (generic externs; methods, found through their impl).
static void RewriteCall(Mono* m, CallExpr* call)
{
    size_t argDepth = 0;

    for (size_t i = 0; i < call->typeArgCount; i++)
    {
        argDepth = MaxSize(argDepth, RewriteType(m, &call->typeArgs[i]));
    }

    RewriteSlot(m, &call->calleeBase);
    RewriteSlotList(m, &call->args);

    if (call->calleeBase || !call->callee)
    {
        return;
    }

    const FunctionDecl* tmpl = (const FunctionDecl*)StrMapGet(&m->functionTemplates, call->callee);

    if (!tmpl)
    {
        if (StrMapGet(&m->structTemplates, call->callee) && call->typeArgCount > 0)
        {
            RewriteGenericStructCall(m, call);
        }
        else if (StrMapGet(&m->structTemplates, call->callee))
        {
            DiagErrorFmt(m->diag, call->base.range, "generic struct '%s' needs type arguments: write '%s<...> { ... }'",
                         call->callee, call->callee);
        }

        return;
    }

    if (call->typeArgCount == 0)
    {
        DiagErrorFmt(m->diag, call->base.range, "generic function '%s' needs explicit type arguments: %s<...>(...)",
                     call->callee, call->callee);
        return;
    }

    if (tmpl->genericParams.count != call->typeArgCount)
    {
        DiagErrorFmt(m->diag, call->base.range, "generic function '%s' takes %zu type argument(s), but %zu were given",
                     call->callee, tmpl->genericParams.count, call->typeArgCount);
        return;
    }

    char* name = TypeNameGenericSpelling(m->arena, call->callee, call->typeArgs, call->typeArgCount);

    if (!WithinLimits(m, argDepth + 1, name, call->callee, call->base.range))
    {
        return;
    }

    InstantiateFunction(m, tmpl, name, call->typeArgs);

    call->callee = name;
    call->typeArg = NULL;
    call->typeArgs = NULL;
    call->typeArgCount = 0;
}

static void RewriteStructInit(Mono* m, StructInitExpr* init)
{
    if (init->genericType)
    {
        RewriteType(m, init->genericType);
        init->typeName = init->genericType->name;
    }
    else if (init->typeName && StrMapGet(&m->structTemplates, init->typeName))
    {
        DiagErrorFmt(m->diag, init->base.range, "generic struct '%s' needs type arguments: write '%s<...> { ... }'",
                     init->typeName, init->typeName);
    }

    for (size_t i = 0; i < init->fields.count; i++)
    {
        RewriteSlot(m, &((StructInitField*)VecGet(&init->fields, i))->value);
    }
}

// The statement a folded `if` leaves in its place: the branch taken, inside a block of its own so that its declarations
// stay in their scope. An empty block when the condition took no branch.
static Node* FoldedBranch(Mono* m, SourceRange range, Node* branch)
{
    if (branch && branch->kind == NodeBlock)
    {
        return branch;
    }

    Block* block = AST_NEW(m->arena, Block);
    block->base.kind = NodeBlock;
    block->base.range = range;
    VecInit(&block->statements);

    if (branch)
    {
        VecPush(&block->statements, branch);
    }

    return (Node*)block;
}

// An `if` statement. When its condition contains a meta call and folds to a constant, the branch not taken is dropped
// before either branch is rewritten, so nothing in it is checked or instantiated.
static void RewriteIf(Mono* m, Node** slot)
{
    IfStmt* s = (IfStmt*)*slot;
    size_t metaCallsBefore = m->metaCalls;

    RewriteSlot(m, &s->condition);

    bool taken = false;

    if (m->metaCalls != metaCallsBefore && FoldCondition(m, s->condition, &taken))
    {
        Node* keep = taken ? s->thenBranch : s->elseBranch;
        Node* drop = taken ? s->elseBranch : s->thenBranch;

        s->thenBranch = NULL;
        s->elseBranch = NULL;
        AstDispose(drop);
        AstDispose((Node*)s);

        *slot = FoldedBranch(m, s->base.range, keep);
        RewriteSlot(m, slot);

        return;
    }

    RewriteSlot(m, &s->thenBranch);
    RewriteSlot(m, &s->elseBranch);
}

// A `cond ? a : b` whose condition folds to a constant is replaced by the branch taken, as RewriteIf does for `if`.
static void RewriteConditional(Mono* m, Node** slot)
{
    ConditionalExpr* e = (ConditionalExpr*)*slot;
    size_t metaCallsBefore = m->metaCalls;

    RewriteSlot(m, &e->condition);

    bool taken = false;

    if (m->metaCalls != metaCallsBefore && FoldCondition(m, e->condition, &taken))
    {
        Node* keep = taken ? e->thenExpr : e->elseExpr;
        Node* drop = taken ? e->elseExpr : e->thenExpr;

        e->thenExpr = NULL;
        e->elseExpr = NULL;
        AstDispose(drop);
        AstDispose((Node*)e);

        *slot = keep;
        RewriteSlot(m, slot);

        return;
    }

    RewriteSlot(m, &e->thenExpr);
    RewriteSlot(m, &e->elseExpr);
}

// Rewrites the statement or expression in `*slot`, which may be replaced. This is the one place where a meta call
// folds, and where an `if` or `?:` whose condition is a constant is pruned. Every slot that can hold one of those
// goes through here, so a replacement always lands in the node that owned it.
static void RewriteSlot(Mono* m, Node** slot)
{
    Node* n = *slot;

    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeMetaCall:
        *slot = FoldMetaCall(m, (MetaCallExpr*)n);
        AstDispose(n);
        return;
    case NodeIf:
        RewriteIf(m, slot);
        return;
    case NodeConditional:
        RewriteConditional(m, slot);
        return;
    default:
        RewriteNode(m, n);
        return;
    }
}

// Rewrites every type and generic use under `n`. Impls only rewrite their properties here: their methods are
// separate function declarations, pushed on their own.
static void RewriteNode(Mono* m, Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeModule:
    case NodeImport:
    case NodeHandle:
    case NodeBreak:
    case NodeContinue:
    case NodeIntLiteral:
    case NodeFloatLiteral:
    case NodeBoolLiteral:
    case NodeStrLiteral:
    case NodeIdent:
        break;
    case NodeIf:
    case NodeConditional:
    case NodeMetaCall:
        // Only reached through RewriteSlot, which replaces them.
        break;
    case NodeStruct:
    {
        StructDecl* sd = (StructDecl*)n;

        for (size_t i = 0; i < sd->fields.count; i++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, i);
            RewriteType(m, &field->type);
            RewriteSlot(m, &field->defaultValue);
        }
        break;
    }
    case NodeEnum:
        RewriteList(m, &((EnumDecl*)n)->members);
        break;
    case NodeEnumMember:
        RewriteSlot(m, &((EnumMemberDecl*)n)->valueExpr);
        break;
    case NodeImpl:
    {
        ImplDecl* impl = (ImplDecl*)n;

        for (size_t i = 0; i < impl->properties.count; i++)
        {
            RewriteType(m, &((PropertyDecl*)VecGet(&impl->properties, i))->returnType);
        }
        break;
    }
    case NodeFunction:
    {
        FunctionDecl* fn = (FunctionDecl*)n;

        RewriteType(m, &fn->returnType);
        RewriteList(m, &fn->params);
        RewriteNode(m, fn->body);
        break;
    }
    case NodeParam:
        RewriteType(m, &((ParamDecl*)n)->type);
        break;
    case NodeBlock:
        RewriteSlotList(m, &((Block*)n)->statements);
        break;
    case NodeReturn:
        RewriteSlot(m, &((ReturnStmt*)n)->value);
        break;
    case NodeWhile:
    {
        WhileStmt* s = (WhileStmt*)n;

        RewriteSlot(m, &s->condition);
        RewriteSlot(m, &s->body);
        break;
    }
    case NodeFor:
    {
        ForStmt* s = (ForStmt*)n;

        RewriteSlot(m, &s->init);
        RewriteSlot(m, &s->condition);
        RewriteSlot(m, &s->update);
        RewriteSlot(m, &s->body);
        break;
    }
    case NodeVarDecl:
    {
        VarDeclStmt* v = (VarDeclStmt*)n;

        // A `var` local's type is still empty before sema; there is nothing to rewrite.
        if (TypeNameValid(&v->type))
        {
            RewriteType(m, &v->type);
        }

        RewriteSlot(m, &v->init);
        break;
    }
    case NodeExprStmt:
        RewriteSlot(m, &((ExprStmt*)n)->expr);
        break;
    case NodeUnary:
        RewriteSlot(m, &((UnaryExpr*)n)->operand);
        break;
    case NodeBinary:
    {
        BinaryExpr* e = (BinaryExpr*)n;

        RewriteSlot(m, &e->lhs);
        RewriteSlot(m, &e->rhs);
        break;
    }
    case NodeAssign:
    {
        AssignExpr* e = (AssignExpr*)n;

        RewriteSlot(m, &e->target);
        RewriteSlot(m, &e->value);
        break;
    }
    case NodeCall:
        RewriteCall(m, (CallExpr*)n);
        break;
    case NodeMember:
        RewriteSlot(m, &((MemberExpr*)n)->baseNode);
        break;
    case NodeStructInit:
        RewriteStructInit(m, (StructInitExpr*)n);
        break;
    case NodeIncDec:
        RewriteSlot(m, &((IncDecExpr*)n)->operand);
        break;
    case NodeCast:
    {
        CastExpr* e = (CastExpr*)n;

        RewriteType(m, &e->type);
        RewriteSlot(m, &e->operand);
        break;
    }
    case NodeGlobal:
    {
        GlobalDecl* g = (GlobalDecl*)n;

        RewriteType(m, &g->type);
        RewriteSlot(m, &g->init);
        break;
    }
    case NodeIndex:
    {
        IndexExpr* e = (IndexExpr*)n;

        RewriteSlot(m, &e->baseNode);
        RewriteSlot(m, &e->index);
        break;
    }
    case NodeArrayInit:
    {
        ArrayInitExpr* e = (ArrayInitExpr*)n;

        // The element type may point into a return or variable type, which is rewritten in place.
        if (e->elementType)
        {
            RewriteType(m, (TypeName*)e->elementType);
        }
        RewriteSlotList(m, &e->elements);
        RewriteSlot(m, &e->repeatCount);
        break;
    }
    case NodeNullTest:
        RewriteSlot(m, &((NullTestExpr*)n)->operand);
        break;
    case NodeDefer:
        RewriteSlot(m, &((DeferStmt*)n)->stmt);
        break;
    }
}

// -- Entry point

void Monomorphize(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Mono m;
    m.mod = mod;
    m.diag = diag;
    m.arena = arena;
    StrMapInit(&m.structTemplates);
    StrMapInit(&m.functionTemplates);
    StrMapInit(&m.structInstances);
    StrMapInit(&m.structDepths);
    StrMapInit(&m.functionInstances);
    VecInit(&m.implTemplates);
    VecInit(&m.pending);
    StrMapInit(&m.metaFunctions);
    m.metaCalls = 0;
    m.instanceCount = 0;
    m.stopped = false;

    Vec templateMethods;
    VecInit(&templateMethods);

    CollectStructTemplates(&m);
    CollectImplTemplates(&m, &templateMethods);
    CollectFunctionTemplates(&m, &templateMethods);
    CheckPlainNames(&m);
    CollectMetaFunctions(&m);
    CheckTemplateTypeKeyword(&m);

    // Every concrete declaration is rewritten once; instances join the work list as they are made.
    PushAll(&m.pending, &mod->structs);
    PushAll(&m.pending, &mod->impls);
    PushAll(&m.pending, &mod->functions);
    PushAll(&m.pending, &mod->globals);
    PushAll(&m.pending, &mod->enums);
    PushAll(&m.pending, &mod->delegates);

    while (m.pending.count > 0)
    {
        RewriteNode(&m, (Node*)VecPop(&m.pending));
    }

    FreeVec(&templateMethods);
    FreeVec(&m.implTemplates);
    FreeVec(&m.pending);
    StrMapFree(&m.structTemplates);
    StrMapFree(&m.functionTemplates);
    StrMapFree(&m.structInstances);
    StrMapFree(&m.structDepths);
    StrMapFree(&m.functionInstances);
    StrMapFree(&m.metaFunctions);
}
