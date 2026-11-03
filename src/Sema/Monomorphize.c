// Monomorphize: expands generic structs, impls and functions into the concrete declarations that use them.
// Sema and codegen only ever see ordinary declarations: the struct `number<int>`, its impl, and the function
// `make_number<int>`. See docs/generics-plan.md, phase 3.
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

// -- Rewriting uses

static void RewriteNode(Mono* m, Node* n);

static void RewriteList(Mono* m, const Vec* list)
{
    for (size_t i = 0; i < list->count; i++)
    {
        RewriteNode(m, (Node*)VecGet(list, i));
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

// A call to a generic function becomes a call to its instance, named by the canonical spelling. Other
// calls are left for sema (generic externs; methods, found through their impl).
static void RewriteCall(Mono* m, CallExpr* call)
{
    size_t argDepth = 0;

    for (size_t i = 0; i < call->typeArgCount; i++)
    {
        argDepth = MaxSize(argDepth, RewriteType(m, &call->typeArgs[i]));
    }

    RewriteNode(m, call->calleeBase);
    RewriteList(m, &call->args);

    if (call->calleeBase || !call->callee)
    {
        return;
    }

    const FunctionDecl* tmpl = (const FunctionDecl*)StrMapGet(&m->functionTemplates, call->callee);

    if (!tmpl)
    {
        if (StrMapGet(&m->structTemplates, call->callee))
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
        RewriteNode(m, ((StructInitField*)VecGet(&init->fields, i))->value);
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
    case NodeStruct:
    {
        StructDecl* sd = (StructDecl*)n;

        for (size_t i = 0; i < sd->fields.count; i++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, i);
            RewriteType(m, &field->type);
            RewriteNode(m, field->defaultValue);
        }
        break;
    }
    case NodeEnum:
        RewriteList(m, &((EnumDecl*)n)->members);
        break;
    case NodeEnumMember:
        RewriteNode(m, ((EnumMemberDecl*)n)->valueExpr);
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
        RewriteList(m, &((Block*)n)->statements);
        break;
    case NodeReturn:
        RewriteNode(m, ((ReturnStmt*)n)->value);
        break;
    case NodeIf:
    {
        IfStmt* s = (IfStmt*)n;

        RewriteNode(m, s->condition);
        RewriteNode(m, s->thenBranch);
        RewriteNode(m, s->elseBranch);
        break;
    }
    case NodeWhile:
    {
        WhileStmt* s = (WhileStmt*)n;

        RewriteNode(m, s->condition);
        RewriteNode(m, s->body);
        break;
    }
    case NodeFor:
    {
        ForStmt* s = (ForStmt*)n;

        RewriteNode(m, s->init);
        RewriteNode(m, s->condition);
        RewriteNode(m, s->update);
        RewriteNode(m, s->body);
        break;
    }
    case NodeVarDecl:
    {
        VarDeclStmt* v = (VarDeclStmt*)n;

        RewriteType(m, &v->type);
        RewriteNode(m, v->init);
        break;
    }
    case NodeExprStmt:
        RewriteNode(m, ((ExprStmt*)n)->expr);
        break;
    case NodeUnary:
        RewriteNode(m, ((UnaryExpr*)n)->operand);
        break;
    case NodeBinary:
    {
        BinaryExpr* e = (BinaryExpr*)n;

        RewriteNode(m, e->lhs);
        RewriteNode(m, e->rhs);
        break;
    }
    case NodeAssign:
    {
        AssignExpr* e = (AssignExpr*)n;

        RewriteNode(m, e->target);
        RewriteNode(m, e->value);
        break;
    }
    case NodeCall:
        RewriteCall(m, (CallExpr*)n);
        break;
    case NodeMember:
        RewriteNode(m, ((MemberExpr*)n)->baseNode);
        break;
    case NodeStructInit:
        RewriteStructInit(m, (StructInitExpr*)n);
        break;
    case NodeIncDec:
        RewriteNode(m, ((IncDecExpr*)n)->operand);
        break;
    case NodeCast:
    {
        CastExpr* e = (CastExpr*)n;

        RewriteType(m, &e->type);
        RewriteNode(m, e->operand);
        break;
    }
    case NodeGlobal:
    {
        GlobalDecl* g = (GlobalDecl*)n;

        RewriteType(m, &g->type);
        RewriteNode(m, g->init);
        break;
    }
    case NodeIndex:
    {
        IndexExpr* e = (IndexExpr*)n;

        RewriteNode(m, e->baseNode);
        RewriteNode(m, e->index);
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
        RewriteList(m, &e->elements);
        RewriteNode(m, e->repeatCount);
        break;
    }
    case NodeNullTest:
        RewriteNode(m, ((NullTestExpr*)n)->operand);
        break;
    case NodeDefer:
        RewriteNode(m, ((DeferStmt*)n)->stmt);
        break;
    case NodeConditional:
    {
        ConditionalExpr* e = (ConditionalExpr*)n;

        RewriteNode(m, e->condition);
        RewriteNode(m, e->thenExpr);
        RewriteNode(m, e->elseExpr);
        break;
    }
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
    m.instanceCount = 0;
    m.stopped = false;

    Vec templateMethods;
    VecInit(&templateMethods);

    CollectStructTemplates(&m);
    CollectImplTemplates(&m, &templateMethods);
    CollectFunctionTemplates(&m, &templateMethods);
    CheckPlainNames(&m);

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
}
