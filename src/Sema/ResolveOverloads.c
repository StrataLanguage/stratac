#include "Sema/ResolveOverloads.h"
#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    Module* m_mod;
    DiagnosticEngine* m_diag;
    TypeRegistry m_registry;
    Arena* m_arena;
    StrMap m_constVars;
    StrMap m_movedBoxes;
    StrMap m_boxGlobals;
    StrMap m_refBoxParams;
    const char* m_currentReturnType;
} Resolver;

static char* Mangle(Arena* arena, const FunctionDecl* f)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, f->name);

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        SbPutc(&sb, '$');
        SbPuts(&sb, p->type.name);
    }

    return SbFinish(&sb, arena);
}

static bool IsDefinedStruct(const TypeRegistry* reg, const char* name)
{
    return TypeRegistryIsUserType(reg, name) && !TypeRegistryIsOpaque(reg, name);
}

static bool IsIncompleteStruct(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque && t->incomplete;
}

/* Move-state tracking for box locals (value 1 = moved, 2 = re-live). */
static bool IsBoxMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)1;
}

static void MarkBoxMoved(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)1);
}

static void MarkBoxLive(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)2);
}

static bool IsBoxGlobalName(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_boxGlobals, name) != NULL;
}

/* The identifier a dotted move key is rooted in - "holder.gun" -> "holder",
   "p" -> "p". Used to check the underlying binding (e.g. a ref box<T>
   param), not just the literal field-access text. */
static const char* KeyRoot(Arena* arena, const char* key)
{
    const char* dot = strchr(key, '.');

    return dot ? arena_strndup(arena, key, (size_t)(dot - key)) : key;
}

/* Marks 'name' moved, unless it's a box global or rooted in a `ref box<T>`
   param - a borrow of the caller's box, which the callee doesn't own and
   can't validly move out of (the caller's own liveness tracking has no way
   to see a move that happens inside a different function). */
static void MoveBoxIdent(Resolver* r, const char* name, SourceRange range)
{
    if (IsBoxGlobalName(r, name))
    {
        DiagErrorFmt(r->m_diag, range,
                     "'%s' cannot be moved as it is not owned because it is global. (use a `[const] ref box<T>` or pass `T` by value)",
                     name, name);

        return;
    }

    const char* root = KeyRoot(r->m_arena, name);

    if (StrMapGet(&r->m_refBoxParams, root))
    {
        DiagErrorFmt(r->m_diag, range, "'%s' cannot be moved as it is not owned because it is bound as a ref.", name);

        return;
    }

    MarkBoxMoved(r, name);
}

/* Move-tracking key for 'n' (unwraps casts), or NULL if not movable.
   Identifier -> its name; member chain -> dotted key ("holder.gun"). */
static const char* MovableBoxSourceKey(Resolver* r, Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    if (!n)
    {
        return NULL;
    }

    if (n->kind == NodeIdent)
    {
        return ((IdentExpr*)n)->name;
    }

    if (n->kind == NodeMember)
    {
        MemberExpr* m = (MemberExpr*)n;
        const char* baseKey = MovableBoxSourceKey(r, m->base_node);

        return baseKey ? arena_format(r->m_arena, "%s.%s", baseKey, m->member) : NULL;
    }

    return NULL;
}

static void CopyStrMap(const StrMap* src, StrMap* dst)
{
    StrMapInit(dst);

    for (size_t i = 0; i < src->cap; i++)
    {
        if (src->keys[i])
        {
            StrMapPut(dst, src->keys[i], src->values[i]);
        }
    }
}

static void ReplaceStrMapContents(StrMap* dst, const StrMap* src)
{
    StrMapClear(dst);

    for (size_t i = 0; i < src->cap; i++)
    {
        if (src->keys[i])
        {
            StrMapPut(dst, src->keys[i], src->values[i]);
        }
    }
}

/* Conservatively folds 'other' into 'dst' (both moved-box state snapshots):
   a name ends up moved if either side has it moved, else live if either
   side has it live, else it's left as dst's value. Used to combine the
   moved-state left by two mutually exclusive if/else branches, since only
   one of them actually runs. */
static void MergeMovedBoxes(StrMap* dst, const StrMap* other)
{
    for (size_t i = 0; i < other->cap; i++)
    {
        if (!other->keys[i])
        {
            continue;
        }

        void* otherVal = other->values[i];
        void* dstVal = StrMapGet(dst, other->keys[i]);

        if (otherVal == (void*)1 || dstVal == (void*)1)
        {
            StrMapPut(dst, other->keys[i], (void*)1);
        }
        else if (otherVal == (void*)2 || dstVal == (void*)2)
        {
            StrMapPut(dst, other->keys[i], (void*)2);
        }
    }
}

static int CountByName(const Module* mod, const char* name)
{
    int count = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(functionDecl->name, name) == 0)
        {
            count++;
        }
    }

    return count;
}

static const char* InferType(Resolver* r, Node* n, StrMap* scope);

static void WalkBlock(Resolver* r, Block* b, StrMap* scope);
static void WalkStmt(Resolver* r, Node* n, StrMap* scope);
static void ResolveExpr(Resolver* r, Node* n, StrMap* scope);
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope);

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range)
{
    Node* base = target;
    while (base->kind == NodeMember)
    {
        base = ((MemberExpr*)base)->base_node;
    }

    if (base->kind == NodeIdent)
    {
        const char* name = ((IdentExpr*)base)->name;
        if (StrMapGet(&r->m_constVars, name))
        {
            DiagErrorFmt(r->m_diag, range, "'%s' is immutable", name);
        }
    }
}

/**
 * @brief Attempt to resolve calls for SIMD types (e.g. `float3(x, y, z)` or `float4(w, x, y, z)`).
 * @returns True if this was a valid constructor or false if it was malformed.
 */
static bool ResolveSimdVectorConstruct(Resolver* r, CallExpr* c, StrMap* scope)
{
    // The function name is the same as the type name, so we can use TypeUtil functions on it.
    int numLanes = GetSimdVectorLanes(c->callee);

    if (numLanes == 0)
    {
        return false;
    }

    // TODO: replace this when adding integer SIMD vectors
    const char* requiredArgType = "float";

    // Calls are valid for scalar splatting (e.g. float3(x)) and loads (e.g. float3(x, y, z))
    const bool isValidCall = (c->args.count == 1 || c->args.count == numLanes);

    if (!isValidCall)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "no matching call to '%s' with %zu arguments", c->callee, c->args.count);
        return false;
    }

    for (size_t i = 0; i < c->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, i);

        const char* argType = InferType(r, arg, scope);

        if (argType[0] == '\0')
        {
            continue;
        }

        if (strcmp(argType, "float") != 0)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "expected argument of type 'float' but found '%s'", argType);
            return false;
        }
    }

    c->isPseudoCall = true;

    return true;
}

//-- Array intrinsics.

/* Result type of an array builtin call, or NULL if `c` isn't one.
   array_push -> ulong (new length); array_pop -> element type; array_resize -> void. */
static const char* ArrayBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return NULL;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    bool isResize = strcmp(c->callee, "array_resize") == 0;
    bool isPop = strcmp(c->callee, "array_pop") == 0;

    if (!isPush && !isResize && !isPop)
    {
        return NULL;
    }

    Node* arg0 = c->args.count > 0 ? (Node*)VecGet(&c->args, 0) : NULL;
    const char* arrType = arg0 ? InferType(r, arg0, scope) : "";
    Str inner = ArrayInnerStr(arrType);

    /* arg0 must be an array for this to be a (valid) builtin call. */
    if (!inner.data)
    {
        return NULL;
    }

    if (isPop)
    {
        return StrNew(r->m_arena, inner.data, inner.len).data;
    }

    return isPush ? "ulong" : "void";
}

static bool IsAssignableType(const Resolver* r, const char* targetType, const char* valueType);

/* True if `n` (after unwrapping casts) is an array-element read like `arr[i]`:
   a borrow whose owner (the array) retains the value. Such a value cannot be
   moved, so pushing it into an array would duplicate ownership. */
static bool IsArrayElementBorrow(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return n && n->kind == NodeIndex;
}

/* Validates an array builtin call and marks it a pseudo-call. Returns true if
   `c` is one of array_push/pop/resize (regardless of whether it was valid). */
static bool ResolveArrayBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return false;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    bool isResize = strcmp(c->callee, "array_resize") == 0;
    bool isPop = strcmp(c->callee, "array_pop") == 0;

    if (!isPush && !isResize && !isPop)
    {
        return false;
    }

    size_t wantArgs = (isPop) ? 1 : 2;

    if (c->args.count != wantArgs)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects %zu argument(s) but got %zu",
                     c->callee, wantArgs, c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    ResolveExpr(r, arg0, scope);

    const char* arrType = InferType(r, arg0, scope);

    if (!IsArrayType(arrType))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' expects an array argument, not '%s'", c->callee, arrType);
        return true;
    }

    /* The array is mutated in place, so it must be addressable (an lvalue). */
    if (arg0->kind != NodeIdent && arg0->kind != NodeMember && arg0->kind != NodeIndex)
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' array argument must be an lvalue", c->callee);
        return true;
    }

    if (isResize)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);
        ResolveExpr(r, arg1, scope);

        const char* sizeType = InferType(r, arg1, scope);

        if (sizeType[0] != '\0' && !IsNumeric(sizeType))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "'array_resize' size must be an integer, not '%s'", sizeType);
        }
    }
    else if (isPush)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);
        ResolveExpr(r, arg1, scope);

        const char* valueType = InferType(r, arg1, scope);

        /* The value must be assignable to the array's element type. */
        Str innerRaw = ArrayInnerStr(arrType);
        const char* elemType = innerRaw.data ? StrNew(r->m_arena, innerRaw.data, innerRaw.len).data : "";

        if (valueType[0] != '\0' && elemType[0] != '\0' && !IsAssignableType(r, elemType, valueType))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push a value of type '%s' into '%s' (element type '%s')",
                         valueType, arrType, elemType);
        }

        /* Pushing an owning value that is itself read out of an array element
           (a borrow) would duplicate the owning pointer - the source array
           keeps it too, so both would free it. Move it into a local first. */
        if (valueType[0] != '\0' && IsOwningType(valueType) && IsArrayElementBorrow(arg1))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push '%s' read from an array element - it would be owned by two arrays; "
                         "move it into a variable first",
                         valueType);
        }

        /* Pushing an owning value (string/box) moves it into the array. */
        if (valueType[0] != '\0' && IsOwningType(valueType))
        {
            const char* movedKey = MovableBoxSourceKey(r, arg1);

            if (movedKey)
            {
                MoveBoxIdent(r, movedKey, c->base.range);
            }
        }
    }

    c->isPseudoCall = true;

    return true;
}
//--

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        return;
    }

    // If the function call is to `float3()` or `float4()`, resolve internally
    if (c->callee != NULL && ResolveSimdVectorConstruct(r, c, scope))
    {
        return;
    }

    // Inline array helpers: array_push / array_pop / array_resize.
    if (c->callee != NULL && ResolveArrayBuiltin(r, c, scope))
    {
        return;
    }

    /* Already resolved on an earlier pass over this same call (e.g. the
       warmup walk in WalkLoopBody) - c->callee has been rewritten to the
       chosen overload's mangled name, so a fresh name lookup below would
       find nothing. Reuse the cached decl and just redo the move-tracking
       side effects, which do need to run again for the real pass. */
    if (c->resolvedDecl)
    {
        const FunctionDecl* best = c->resolvedDecl;

        for (size_t j = 0; j < c->args.count && j < best->params.count; j++)
        {
            ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            const char* movedArgKey = IsOwningType(param->type.name) && param->mod == ModNone && !best->isExtern
                                          ? MovableBoxSourceKey(r, arg)
                                          : NULL;

            if (movedArgKey)
            {
                MoveBoxIdent(r, movedArgKey, arg->range);
            }
        }

        return;
    }

    bool found = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&r->m_mod->functions, i);

        if (strcmp(functionDecl->name, c->callee) == 0)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "unknown function '%s'", c->callee);
        return;
    }

    FunctionDecl* best = NULL;

    int bestScore = INT_MAX;
    bool ambiguous = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&r->m_mod->functions, i);

        if (strcmp(functionDecl->name, c->callee) != 0)
        {
            continue;
        }

        if (functionDecl->params.count != c->args.count)
        {
            continue;
        }

        int score = 0;

        bool viable = true;

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);
            const char* argType = InferType(r, arg, scope);
            ParamDecl* param = (ParamDecl*)VecGet(&functionDecl->params, j);

            if (argType[0] == '\0')
            {
                continue;
            }

            if (strcmp(argType, param->type.name) == 0)
            {
            }
            else if (IsNumeric(argType) && IsNumeric(param->type.name))
            {
                score += 1;
            }
            else if (IsSimdVector(argType) && IsSimdVector(param->type.name))
            {
                score += 1;
            }
            else if (HandleExtendsFrom(&r->m_registry, argType, param->type.name))
            {
                score += 1;
            }
            else if (IsOwningType(argType))
            {
                /* box<T> coerces to T (implicit deref). */
                if (StrEqC(OwningInnerStr(argType), param->type.name))
                {
                    score += 1;
                }
                else
                {
                    viable = false;
                    break;
                }
            }
            else
            {
                viable = false;

                break;
            }
        }

        if (!viable)
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

    if (!best)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "no matching overload for '%s' with %zu argument(s)", c->callee,
                     c->args.count);
        return;
    }

    if (ambiguous)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "ambiguous call to overload '%s'", c->callee);
    }

    c->callee = best->mangledName;
    c->resolvedDecl = best;

    /* Passing a box to an owned (non-ref) box parameter moves it. */
    for (size_t j = 0; j < c->args.count && j < best->params.count; j++)
    {
        ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);
        Node* arg = (Node*)VecGet(&c->args, j);

        const char* movedArgKey = IsOwningType(param->type.name) && param->mod == ModNone && !best->isExtern
                                      ? MovableBoxSourceKey(r, arg)
                                      : NULL;

        if (movedArgKey)
        {
            MoveBoxIdent(r, movedArgKey, arg->range);
        }
    }
}

static const char* InferType(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return "";
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    {
        const IntLiteral* lit = (const IntLiteral*)n;

        if (lit->value > 0xFFFFFFFFULL)
        {
            return lit->isUnsigned ? "ulong" : "long";
        }

        return lit->isUnsigned ? "uint" : "int";
    }
    case NodeFloatLiteral:
        return "float";
    case NodeBoolLiteral:
        return "bool";
    case NodeStrLiteral:
        return "string";
    case NodeIdent:
    {
        const char* t = (const char*)StrMapGet(scope, ((IdentExpr*)n)->name);

        return t ? t : "";
    }
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;

        return u->op == UnNot ? "bool" : InferType(r, u->operand, scope);
    }
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        switch (b->op)
        {
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
        case BinLogicAnd:
        case BinLogicOr:
            return "bool";
        default:
        {
            const char* lt = InferType(r, b->lhs, scope);
            const char* rt = InferType(r, b->rhs, scope);

            if (strcmp(lt, "double") == 0 || strcmp(rt, "double") == 0)
            {
                return "double";
            }

            if (strcmp(lt, "float") == 0 || strcmp(rt, "float") == 0)
            {
                return "float";
            }

            if (strcmp(lt, "ulong") == 0 || strcmp(rt, "ulong") == 0)
            {
                return "ulong";
            }

            if (strcmp(lt, "long") == 0 || strcmp(rt, "long") == 0)
            {
                return "long";
            }

            if (strcmp(lt, "uint") == 0 || strcmp(rt, "uint") == 0)
            {
                return "uint";
            }

            return "int";
        }
        }
    }
    case NodeAssign:
        return InferType(r, ((AssignExpr*)n)->target, scope);
    case NodeIncDec:
        return InferType(r, ((IncDecExpr*)n)->operand, scope);
    case NodeCast:
        return ((CastExpr*)n)->type.name;
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;

        const char* baseName = InferType(r, m->base_node, scope);

        const char* _inner = OwningInnerCStr(r->m_arena, baseName);
        if (_inner)
        {
            baseName = _inner;
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseName))
        {
            if (IsIncompleteStruct(&r->m_registry, baseName))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of incomplete type '%s'", m->member,
                             baseName);
            }
            else
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of opaque handle '%s'", m->member,
                             baseName);
            }

            return "";
        }

        const StructType* structType = TypeRegistryFind(&r->m_registry, baseName);
        if (!structType)
        {
            return "";
        }

        int idx = TypeRegistryFieldIndex(&r->m_registry, baseName, m->member);
        if (idx < 0)
        {
            return "";
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet((Vec*)&structType->fields, (size_t)idx);

        return fieldDecl->type.name;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;

        if (c->resolvedDecl)
        {
            return c->resolvedDecl->returnType.name;
        }

        const char* builtinType = ArrayBuiltinType(r, c, scope);

        if (builtinType)
        {
            return builtinType;
        }

        if (TypeRegistryIsUserType(&r->m_registry, c->callee))
        {
            return c->callee;
        }

        return "";
    }
    case NodeStructInit:
        return ((StructInitExpr*)n)->typeName;
    case NodeIndex:
    {
        const char* baseName = InferType(r, ((IndexExpr*)n)->base_node, scope);
        Str inner = ArrayInnerStr(baseName);

        return inner.data ? StrNew(r->m_arena, inner.data, inner.len).data : "";
    }
    case NodeArrayInit:
    {
        const char* et = ((ArrayInitExpr*)n)->elementType;
        return (et && et[0]) ? arena_format(r->m_arena, "%s[]", et) : "";
    }
    default:
        return "";
    }
}

static bool IsAssignableType(const Resolver* r, const char* targetType, const char* valueType)
{
    if (IsOwningType(targetType))
    {
        Str inner = OwningInnerStr(targetType);

        return StrEqC(inner, valueType) || strcmp(valueType, targetType) == 0;
    }

    // Assignable if type is exact match
    if (strcmp(valueType, targetType) == 0)
    {
        return true;
    }

    // If both types are numeric (and therefore convertible)
    // TODO: Require explicit casts when performing lossy conversions (e.g. ulong to uint)
    if (IsNumeric(valueType) && IsNumeric(targetType))
    {
        return true;
    }

    // Assignment to a SIMD vector (vector = scalar, vector = vector)
    if (IsSimdVector(targetType) && (IsSimdVector(valueType) || IsNumeric(valueType)))
    {
        return true;
    }

    return HandleExtendsFrom(&r->m_registry, valueType, targetType) || StrEqC(OwningInnerStr(valueType), targetType);
}

static void ResolveExpr(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    case NodeFloatLiteral:
    case NodeBoolLiteral:
    case NodeStrLiteral:
        return;
    case NodeIdent:
    {
        IdentExpr* ident = (IdentExpr*)n;
        const char* varType = (const char*)StrMapGet(scope, ident->name);

        if (!varType)
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "unknown variable '%s'", ident->name);
        }
        else if (IsOwningType(varType) && IsBoxMoved(r, ident->name))
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "'%s' used after move", ident->name);
        }

        return;
    }
    case NodeUnary:
        ResolveExpr(r, ((UnaryExpr*)n)->operand, scope);
        return;
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        ResolveExpr(r, b->lhs, scope);
        ResolveExpr(r, b->rhs, scope);

        return;
    }
    case NodeAssign:
    {
        AssignExpr* a = (AssignExpr*)n;

        CheckConstAssign(r, a->target, a->base.range);

        const char* tt = (a->target->kind == NodeIdent) ? InferType(r, a->target, scope) : NULL;
        bool targetIsBox = tt && IsOwningType(tt);

        if (targetIsBox)
        {
            /* A bare braced array literal RHS (`a = {1,2,3};`) carries no
               element type from the parser - infer it from the array target
               so codegen and the type checks below see a real type. */
            if (IsArrayType(tt) && a->value->kind == NodeArrayInit)
            {
                ArrayInitExpr* ai = (ArrayInitExpr*)a->value;

                if (ai->elementType[0] == '\0')
                {
                    Str inner = ArrayInnerStr(tt);
                    ai->elementType = StrNew(r->m_arena, inner.data, inner.len).data;
                }
            }

            /* Resolve the value first (so a call gets its resolvedDecl set)
               before inferring its type - otherwise an unresolved call's
               type reads as "", misclassifying the assignment below. */
            ResolveExpr(r, a->value, scope);
            const char* vt = InferType(r, a->value, scope);

            /* `=` rebinds the box (moves in a new box of the same type);
               any other assignment into a box<T> - including `=` with a
               plain T value, e.g. `x = 5;` - mutates its contents instead. */
            bool boxMove = a->op == AssignSet && vt[0] != '\0' && strcmp(vt, tt) == 0;
            const char* targetName = ((IdentExpr*)a->target)->name;

            if (boxMove)
            {
                if (IsBoxGlobalName(r, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "box global '%s' cannot be reassigned; only its fields may be mutated", targetName);

                    return;
                }

                /* A `ref box<T>` is a view of the caller's box binding, not
                   something this function owns - rebinding it (`inBox =
                   otherBox;`) would silently rebind the caller's variable
                   too. Only the box's contents can be assigned through a
                   ref (`inBox = someInnerValue;`), same restriction as a
                   box global. */
                if (StrMapGet(&r->m_refBoxParams, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "'%s' cannot be reassigned as it is not owned because it is bound as a ref; "
                                 "assign its inner value instead",
                                 targetName);

                    return;
                }

                MarkBoxLive(r, targetName);

                const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                if (movedValueKey)
                {
                    MoveBoxIdent(r, movedValueKey, a->base.range);
                }
            }
            else
            {
                /* Assigning a plain T (or compound-assigning) into a
                   box<T> (`x = 5;`, `val -= amt;`) mutates the boxed value
                   in place - not a move, so it's allowed even for a box
                   global or a moved-and-revalidated box. */
                if (IsBoxMoved(r, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "'%s'  used after move", targetName);
                }

                const char* inner = OwningInnerCStr(r->m_arena, tt);

                if (vt[0] != '\0' && !IsAssignableType(r, inner, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' into box<%s> '%s'", vt, inner,
                                 targetName);
                }
            }
        }
        else
        {
            ResolveExpr(r, a->target, scope);
            ResolveExpr(r, a->value, scope);

            /* A box<T> value assigned into a plain (non-box) T target - a
               ref T param, a local, a field - reads through the box (same
               "box<T> -> T" coercion already used for var-decl inits, call
               args, and returns), not a move. */
            if (tt)
            {
                const char* vt = InferType(r, a->value, scope);

                if (vt[0] != '\0' && !IsAssignableType(r, tt, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to '%s' of type '%s'", vt,
                                 ((IdentExpr*)a->target)->name, tt);
                }
            }
        }

        return;
    }
    case NodeIncDec:
    {
        IncDecExpr* inc = (IncDecExpr*)n;
        CheckConstAssign(r, inc->operand, inc->base.range);
        ResolveExpr(r, inc->operand, scope);
        return;
    }
    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;
        ResolveExpr(r, cast->operand, scope);

        const char* src = InferType(r, cast->operand, scope);
        const char* dst = cast->type.name;

        bool scalarPair = src && dst && IsScalarTypeName(src) && IsScalarTypeName(dst);
        bool handlePair
            = src && dst && IsHandleType(&r->m_registry, src) && IsHandleType(&r->m_registry, dst)
              && (HandleExtendsFrom(&r->m_registry, dst, src) || HandleExtendsFrom(&r->m_registry, src, dst));

        /* box<T> -> box<U> only when T or U is opaque (erase/cast-back). */
        bool boxPair = src && dst && IsOwningType(src) && IsOwningType(dst)
                       && (TypeRegistryIsOpaque(&r->m_registry, OwningInnerCStr(r->m_arena, src))
                           || TypeRegistryIsOpaque(&r->m_registry, OwningInnerCStr(r->m_arena, dst)));

        if (src && src[0] != '\0' && dst && dst[0] != '\0' && !scalarPair && !handlePair && !boxPair)
        {
            DiagErrorFmt(r->m_diag, cast->base.range, "invalid cast from '%s' to '%s'", src, dst);
        }

        return;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;
        ResolveExpr(r, m->base_node, scope);

        const char* baseName = InferType(r, m->base_node, scope);

        const char* _inner = OwningInnerCStr(r->m_arena, baseName);
        if (_inner)
        {
            baseName = _inner;
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseName))
        {
            if (IsIncompleteStruct(&r->m_registry, baseName))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'", baseName);
            }
            else
            {
                DiagError(r->m_diag, m->base.range, "cannot access a member of an opaque handle");
            }
        }

        /* A box-typed field can be moved out too - check the same way. */
        if (IsOwningType(InferType(r, n, scope)))
        {
            const char* key = MovableBoxSourceKey(r, n);

            if (key && IsBoxMoved(r, key))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "'%s' used after move", key);
            }
        }

        return;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;

        for (size_t i = 0; i < c->args.count; i++)
        {
            ResolveExpr(r, (Node*)VecGet(&c->args, i), scope);
        }

        ResolveCall(r, c, scope);

        return;
    }
    case NodeStructInit:
    {
        StructInitExpr* structInitExpr = (StructInitExpr*)n;

        if (!TypeRegistryIsUserType(&r->m_registry, structInitExpr->typeName))
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range, "'%s' is not a known aggregate type",
                         structInitExpr->typeName);
        }
        else if (TypeRegistryIsOpaque(&r->m_registry, structInitExpr->typeName))
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range, "'%s' is opaque and may not be instantiated",
                         structInitExpr->typeName);
        }

        size_t positionalCount = 0;

        const StructType* structType = TypeRegistryFind(&r->m_registry, structInitExpr->typeName);

        for (size_t i = 0; i < structInitExpr->fields.count; i++)
        {
            StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);
            ResolveExpr(r, field->value, scope);

            FieldDecl* fieldDecl = NULL;

            if (field->name && field->name[0] != '\0')
            {
                int idx
                    = structType ? TypeRegistryFieldIndex(&r->m_registry, structInitExpr->typeName, field->name) : -1;

                if (structType && idx < 0)
                {
                    DiagErrorFmt(r->m_diag, structInitExpr->base.range, "struct '%s' has no field named '%s'",
                                 structInitExpr->typeName, field->name);
                }
                else if (structType)
                {
                    fieldDecl = (FieldDecl*)VecGet(&structType->fields, (size_t)idx);
                }
            }
            else
            {
                if (structType && positionalCount >= structType->fields.count)
                {
                    DiagErrorFmt(r->m_diag, structInitExpr->base.range, "too many initializers for struct '%s'",
                                 structInitExpr->typeName);
                }
                else if (structType)
                {
                    fieldDecl = (FieldDecl*)VecGet(&structType->fields, positionalCount);
                }

                positionalCount++;
            }

            if (fieldDecl)
            {
                const char* fieldValueType = InferType(r, field->value, scope);

                if (fieldValueType[0] != '\0' && !IsAssignableType(r, fieldDecl->type.name, fieldValueType))
                {
                    DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                                 "field '%s' of struct '%s' cannot be initialized by expression of type '%s'",
                                 fieldDecl->name, structInitExpr->typeName, fieldValueType);
                }

                /* A box<T> field moves its source (identifier/field/cast). */
                const char* movedFieldKey
                    = IsOwningType(fieldDecl->type.name) ? MovableBoxSourceKey(r, field->value) : NULL;

                if (movedFieldKey)
                {
                    MoveBoxIdent(r, movedFieldKey, structInitExpr->base.range);
                }
            }
        }

        return;
    }
    case NodeIndex:
    {
        IndexExpr* ix = (IndexExpr*)n;
        ResolveExpr(r, ix->base_node, scope);
        ResolveExpr(r, ix->index, scope);
        return;
    }
    case NodeArrayInit:
    {
        ArrayInitExpr* ai = (ArrayInitExpr*)n;

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            ResolveExpr(r, (Node*)VecGet(&ai->elements, i), scope);
        }

        return;
    }
    default:
        return;
    }
}

/* A loop body runs repeatedly, so a box move it contains must stay valid
   across the "loop back" edge too - not just top-to-bottom once. Walk the
   body once with diagnostics suppressed to propagate the moved-state one
   iteration would leave behind (this is what the next iteration actually
   starts from), then walk it again for real; any move violation that only
   shows up because of that carried-over state - like moving the same box
   on every iteration of a loop - is caught by the second walk. */
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope)
{
    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    WalkStmt(r, body, scope);

    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    WalkStmt(r, body, scope);
}

static void WalkStmt(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        WalkBlock(r, (Block*)n, scope);
        return;

    case NodeVarDecl:
    {
        VarDeclStmt* vd = (VarDeclStmt*)n;

        if (IsIncompleteStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, vd->type.name);
        }

        /* Owning types must be initialized so they hold a valid heap pointer
           - except arrays, which default to an empty {null, 0} fat struct. */
        if (IsOwningType(vd->type.name) && !IsArrayType(vd->type.name) && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "box variable '%s' must be initialized", vd->name);
        }

        if (vd->type.isConst && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "const variable '%s' must be initialized", vd->name);
        }

        if (TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "owning struct '%s' must be stored in a box; use 'box<%s>'",
                         vd->type.name, vd->type.name);
        }

        /* An owning struct held by value as an array element would leak its
           owning fields on drop - it must be boxed too (box<S>[]). */
        {
            Str arrInner = ArrayInnerStr(vd->type.name);

            if (arrInner.data && TypeRegistryIsOwningStruct(&r->m_registry, StrNew(r->m_arena, arrInner.data, arrInner.len).data))
            {
                const char* innerC = StrNew(r->m_arena, arrInner.data, arrInner.len).data;
                DiagErrorFmt(r->m_diag, vd->base.range,
                             "owning struct '%s' must be stored in a box; use 'box<%s>[]'",
                             innerC, innerC);
            }
        }

        if (vd->init)
        {
            ResolveExpr(r, vd->init, scope);
            const char* initType = InferType(r, vd->init, scope);

            bool ok = IsAssignableType(r, vd->type.name, initType);

            if (initType[0] != '\0' && !ok)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "'%s' cannot be initialized by expression of type '%s'",
                             vd->type.name, initType);
            }

            /* box<T> init from a box source (identifier/field/cast) moves it. */
            const char* movedInitKey
                = IsOwningType(vd->type.name) && IsOwningType(initType) ? MovableBoxSourceKey(r, vd->init) : NULL;

            if (movedInitKey)
            {
                MoveBoxIdent(r, movedInitKey, vd->base.range);
            }
        }

        /* Fresh binding: clear any stale moved-state a same-named variable
           left behind (e.g. from a prior loop iteration or an earlier,
           unrelated declaration in this function). */
        StrMapPut(&r->m_movedBoxes, vd->name, NULL);
        StrMapPut(scope, vd->name, (void*)vd->type.name);

        return;
    }
    case NodeExprStmt:
        ResolveExpr(r, ((ExprStmt*)n)->expr, scope);
        return;
    case NodeReturn:
    {
        ReturnStmt* rs = (ReturnStmt*)n;
        if (rs->value)
        {
            if (r->m_currentReturnType && strcmp(r->m_currentReturnType, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "void function cannot return a value");
            }

            ResolveExpr(r, rs->value, scope);

            const char* typeName = InferType(r, rs->value, scope);

            if (typeName[0] != '\0' && strcmp(typeName, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type 'void'");
            }

            /* Only a move if the function itself returns box<T>; otherwise
               it's a read (unless the inner type is owning). */
            const char* movedReturnKey = IsOwningType(typeName) ? MovableBoxSourceKey(r, rs->value) : NULL;

            if (movedReturnKey)
            {
                bool returnsSameBox = r->m_currentReturnType && strcmp(r->m_currentReturnType, typeName) == 0;

                if (returnsSameBox)
                {
                    MoveBoxIdent(r, movedReturnKey, rs->base.range);
                }
                else
                {
                    const char* boxInner = OwningInnerCStr(r->m_arena, typeName);

                    bool innerIsOwning = TypeRegistryIsOwningStruct(&r->m_registry, boxInner);
                    bool innerMatchesReturn = boxInner
                                              && (r->m_currentReturnType
                                                  && (strcmp(r->m_currentReturnType, boxInner) == 0
                                                      || (IsNumeric(boxInner) && IsNumeric(r->m_currentReturnType))));

                    if (innerIsOwning || !innerMatchesReturn)
                    {
                        MoveBoxIdent(r, movedReturnKey, rs->base.range);
                    }
                }
            }
        }

        return;
    }
    case NodeIf:
    {
        IfStmt* i = (IfStmt*)n;
        ResolveExpr(r, i->condition, scope);

        /* then/else are mutually exclusive - each is walked from the same
           starting moved-state (not one after the other), and the state
           after the if is a conservative merge of both outcomes. */
        StrMap beforeBranches;
        CopyStrMap(&r->m_movedBoxes, &beforeBranches);

        WalkStmt(r, i->thenBranch, scope);

        StrMap afterThen;
        CopyStrMap(&r->m_movedBoxes, &afterThen);

        ReplaceStrMapContents(&r->m_movedBoxes, &beforeBranches);

        if (i->elseBranch)
        {
            WalkStmt(r, i->elseBranch, scope);
        }

        MergeMovedBoxes(&r->m_movedBoxes, &afterThen);

        StrMapFree(&beforeBranches);
        StrMapFree(&afterThen);

        return;
    }
    case NodeWhile:
    {
        WhileStmt* w = (WhileStmt*)n;
        ResolveExpr(r, w->condition, scope);
        WalkLoopBody(r, w->body, scope);

        return;
    }
    case NodeFor:
    {
        ForStmt* fs = (ForStmt*)n;

        if (fs->init)
        {
            if (fs->init->kind == NodeVarDecl)
            {
                WalkStmt(r, fs->init, scope);
            }
            else
            {
                ResolveExpr(r, fs->init, scope);
            }
        }

        if (fs->condition)
        {
            ResolveExpr(r, fs->condition, scope);
        }

        if (fs->update)
        {
            ResolveExpr(r, fs->update, scope);
        }

        WalkLoopBody(r, fs->body, scope);

        return;
    }
    default:
        return;
    }
}

static void WalkBlock(Resolver* r, Block* b, StrMap* scope)
{
    for (size_t i = 0; i < b->statements.count; i++)
    {
        WalkStmt(r, (Node*)VecGet(&b->statements, i), scope);
    }
}

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Resolver r = {0};
    r.m_mod = mod;
    r.m_diag = diag;
    r.m_arena = arena;

    TypeRegistryInit(&r.m_registry);
    TypeRegistryBuild(&r.m_registry, mod);

    StrMapInit(&r.m_constVars);
    StrMapInit(&r.m_movedBoxes);
    StrMapInit(&r.m_boxGlobals);
    StrMapInit(&r.m_refBoxParams);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (IsOwningType(gd->type.name))
        {
            StrMapPut(&r.m_boxGlobals, gd->name, (void*)gd->type.name);
        }
    }

    StrMap byMangled;
    StrMapInit(&byMangled);

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        bool overloaded = CountByName(mod, functionDecl->name) > 1;

        if (overloaded && functionDecl->isExtern)
        {
            DiagErrorFmt(diag, functionDecl->base.range, "extern function '%s' cannot be overloaded",
                         functionDecl->name);
        }

        functionDecl->mangledName = overloaded ? Mangle(arena, functionDecl) : functionDecl->name;

        if (StrMapGet(&byMangled, functionDecl->mangledName))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "duplicate function signature for '%s'", functionDecl->name);
        }

        StrMapPut(&byMangled, functionDecl->mangledName, functionDecl);
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (!functionDecl->body)
        {
            continue;
        }

        StrMap scope;
        StrMapInit(&scope);

        for (size_t j = 0; j < mod->globals.count; j++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, j);
            StrMapPut(&scope, gd->name, (void*)gd->type.name);

            if (gd->type.isConst)
            {
                StrMapPut(&r.m_constVars, gd->name, (void*)1);
            }
        }

        if (r.m_constVars.cap > 0)
        {
            memset(r.m_constVars.keys, 0, r.m_constVars.cap * sizeof(const char*));
            r.m_constVars.count = 0;
        }

        if (r.m_movedBoxes.cap > 0)
        {
            memset(r.m_movedBoxes.keys, 0, r.m_movedBoxes.cap * sizeof(const char*));
            r.m_movedBoxes.count = 0;
        }

        if (r.m_refBoxParams.cap > 0)
        {
            memset(r.m_refBoxParams.keys, 0, r.m_refBoxParams.cap * sizeof(const char*));
            r.m_refBoxParams.count = 0;
        }

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);
            StrMapPut(&scope, p->name, (void*)p->type.name);

            if (p->type.isConst)
            {
                StrMapPut(&r.m_constVars, p->name, (void*)1);
            }

            if (IsOwningType(p->type.name) && p->mod == ModRef)
            {
                StrMapPut(&r.m_refBoxParams, p->name, (void*)1);
            }
        }

        r.m_currentReturnType = functionDecl->returnType.name;

        WalkBlock(&r, (Block*)functionDecl->body, &scope);
        r.m_currentReturnType = NULL;

        StrMapFree(&scope);
    }

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        if (sd->incomplete)
        {
            continue;
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (IsIncompleteStruct(&r.m_registry, field->type.name))
            {
                DiagErrorFmt(diag, field->type.range, "field '%s' has incomplete type '%s'", field->name,
                             field->type.name);
            }
        }
    }

    {
        StrMap globalScope;
        StrMapInit(&globalScope);

        for (size_t i = 0; i < mod->globals.count; i++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);
            StrMapPut(&globalScope, gd->name, (void*)gd->type.name);
        }

        /* Clear per-function move state left over from the last function walked. */
        if (r.m_movedBoxes.cap > 0)
        {
            memset(r.m_movedBoxes.keys, 0, r.m_movedBoxes.cap * sizeof(const char*));
            r.m_movedBoxes.count = 0;
        }

        for (size_t i = 0; i < mod->globals.count; i++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

            /* Array globals default to an empty {null, 0} fat struct. */
            if (IsArrayType(gd->type.name))
            {
                if (gd->init)
                {
                    ResolveExpr(&r, gd->init, &globalScope);
                    const char* initType = InferType(&r, gd->init, &globalScope);

                    if (initType[0] != '\0' && strcmp(initType, gd->type.name) != 0)
                    {
                        DiagErrorFmt(diag, gd->base.range, "global '%s' of type '%s' cannot be initialized by expression of type '%s'",
                                     gd->name, gd->type.name, initType);
                    }
                }

                continue;
            }

            if (!IsOwningType(gd->type.name))
            {
                continue;
            }

            if (!gd->init)
            {
                DiagErrorFmt(diag, gd->base.range, "box global '%s' must be initialized", gd->name);
                continue;
            }

            const char* boxInner = OwningInnerCStr(arena, gd->type.name);

            /* An owning global can't be initialized by moving a source. */
            if (MovableBoxSourceKey(&r, gd->init))
            {
                DiagErrorFmt(diag, gd->base.range,
                             "global '%s' cannot be initialized by moving from another variable; "
                             "initialize it with a value of '%s' or a call returning '%s'",
                             gd->name, gd->type.name, gd->type.name);
                continue;
            }

            ResolveExpr(&r, gd->init, &globalScope);

            const char* initType = InferType(&r, gd->init, &globalScope);

            /* boxInner is NULL for `string` (no box inner), so only compare
               against it when present; otherwise the bare type must match. */
            bool ok = (boxInner && strcmp(initType, boxInner) == 0) || strcmp(initType, gd->type.name) == 0;

            if (initType[0] != '\0' && !ok)
            {
                DiagErrorFmt(diag, gd->base.range, "global '%s' cannot be initialized by expression of type '%s'",
                             gd->name, initType);
            }
        }

        StrMapFree(&globalScope);
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);

            /* `const ref T` is a non-owning view to an immutable. For structs
               (always passed by reference) it is equivalent to `const T`, so we
               allow the explicit spelling rather than rejecting it. */
            (void)p;
        }

        if (functionDecl->isExtern && IsDefinedStruct(&r.m_registry, functionDecl->returnType.name))
        {
            DiagError(diag, functionDecl->base.range, "extern function cannot return a struct type by value");
        }

        if (IsIncompleteStruct(&r.m_registry, functionDecl->returnType.name))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "function cannot return incomplete type '%s'",
                         functionDecl->returnType.name);
        }
    }

    StrMapFree(&r.m_constVars);
    StrMapFree(&r.m_movedBoxes);
    StrMapFree(&r.m_boxGlobals);
    StrMapFree(&r.m_refBoxParams);
    StrMapFree(&byMangled);
    TypeRegistryFree(&r.m_registry);
}
