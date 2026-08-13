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

        /* Distinguish a variadic tail (T...) from a plain T[] param. */
        if (p->isVarargRest)
        {
            SbPuts(&sb, "...");
        }
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

/* Move-state tracking for box locals.
 *   value 1 = fully moved (value/field is gone)
 *   value 2 = re-live (reassigned as a whole)
 *   value 3 = partially moved (a descendant field was moved out; whole-
 *             value use is rejected but non-moved fields are accessible)
 */
static bool IsBoxMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)1;
}

static void MarkBoxMoved(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)1);
}

/* A value is unusable as a whole if fully moved (1) or partially moved (3).
   Individual non-moved field access into a partially-moved value is still
   allowed (handled by the context-sensitive base resolution). */
static bool IsBoxUnusable(const Resolver* r, const char* name)
{
    void* v = StrMapGet(&r->m_movedBoxes, name);
    return v == (void*)1 || v == (void*)3;
}

/* A descendant field was moved out of `name`, but `name` itself was not moved
   as a whole: non-moved fields are still accessible, only whole-value use is
   rejected. */
static bool IsBoxPartiallyMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)3;
}

/* Clears `key` and all its `key.*` descendants from the move-state map.
   Used when a whole-value reassignment re-lives the binding. */
static void ClearBoxSubtree(Resolver* r, const char* key)
{
    StrMap* m = &r->m_movedBoxes;
    size_t klen = strlen(key);

    for (size_t i = 0; i < m->cap; i++)
    {
        if (!m->keys[i])
        {
            continue;
        }

        const char* k = m->keys[i];

        if (strcmp(k, key) == 0 ||
            (strncmp(k, key, klen) == 0 && k[klen] == '.'))
        {
            m->values[i] = NULL;
        }
    }
}

static void MarkBoxLive(Resolver* r, const char* name)
{
    ClearBoxSubtree(r, name);
    StrMapPut(&r->m_movedBoxes, name, (void*)2);
}

/* Reassigning a single owning field (e.g. `bob.name = ...`) re-lives that
   field. Each partially-moved (3) ancestor prefix whose moved descendants
   have all been restored is cleared too, so a whole-value use of the parent
   becomes valid again once every moved field has been reassigned. */
static void RevalidateOwningField(Resolver* r, const char* key)
{
    ClearBoxSubtree(r, key);

    StrMap* m = &r->m_movedBoxes;

    /* Walk every dotted ancestor prefix of `key` (bob.name -> bob;
       holder.gun.ammo -> holder.gun, holder). */
    for (const char* dot = strchr(key, '.'); dot; dot = strchr(dot + 1, '.'))
    {
        const char* prefix = arena_strndup(r->m_arena, key, (size_t)(dot - key));

        /* Only a partially-moved (3) ancestor can be restored this way; a
           fully-moved (1) or re-live (2) one is left untouched. */
        if (StrMapGet(m, prefix) != (void*)3)
        {
            continue;
        }

        size_t plen = strlen(prefix);
        bool hasMovedDescendant = false;

        for (size_t i = 0; i < m->cap; i++)
        {
            const char* k = m->keys[i];

            if (!k || m->values[i] == NULL)
            {
                continue;
            }

            if ((m->values[i] == (void*)1 || m->values[i] == (void*)3)
                && strncmp(k, prefix, plen) == 0 && k[plen] == '.')
            {
                hasMovedDescendant = true;
                break;
            }
        }

        if (!hasMovedDescendant)
        {
            StrMapPut(m, prefix, NULL);
        }
    }
}

static bool IsBoxGlobalName(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_boxGlobals, name) != NULL;
}

/* The identifier a move key is rooted in - "holder.gun" -> "holder",
   "arr[]" -> "arr", "g.arr[]" -> "g". Used to check the underlying binding
   (a global, or a ref box<T> param), not just the literal access text, so
   the move ban applies transitively through struct fields and array
   elements. */
static const char* KeyRoot(Arena* arena, const char* key)
{
    const char* dot = strchr(key, '.');
    const char* base = dot ? arena_strndup(arena, key, (size_t)(dot - key)) : key;

    size_t len = strlen(base);

    if (len >= 2 && base[len - 1] == ']' && base[len - 2] == '[')
    {
        return arena_strndup(arena, base, len - 2);
    }

    return base;
}

/* Marks every proper dotted prefix of `key` as partially moved (value 3),
   unless already fully moved (1). For "a.b.c": marks "a.b" and "a". */
static void MarkBoxPartiallyMoved(Resolver* r, const char* key)
{
    for (const char* dot = strchr(key, '.'); dot; dot = strchr(dot + 1, '.'))
    {
        char* prefix = arena_strndup(r->m_arena, key, (size_t)(dot - key));

        if (StrMapGet(&r->m_movedBoxes, prefix) != (void*)1)
        {
            StrMapPut(&r->m_movedBoxes, prefix, (void*)3);
        }
    }
}

/* Marks 'name' moved, unless it's a box global or rooted in a `ref box<T>`
   param - a borrow of the caller's box, which the callee doesn't own and
   can't validly move out of (the caller's own liveness tracking has no way
   to see a move that happens inside a different function). */
static void MoveBoxIdent(Resolver* r, const char* name, SourceRange range)
{
    const char* root = KeyRoot(r->m_arena, name);

    if (IsBoxGlobalName(r, root))
    {
        DiagErrorFmt(r->m_diag, range,
                     "'%s' cannot be moved as it is not owned because it is global. (use a `[const] ref box<T>`, copy() it, or pass `T` by value)",
                     name);

        return;
    }

    if (StrMapGet(&r->m_refBoxParams, root))
    {
        DiagErrorFmt(r->m_diag, range, "'%s' cannot be moved as it is not owned because it is bound as a ref.", name);

        return;
    }

    MarkBoxMoved(r, name);
    MarkBoxPartiallyMoved(r, name);
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

    if (n->kind == NodeIndex)
    {
        IndexExpr* ix = (IndexExpr*)n;
        const char* baseKey = MovableBoxSourceKey(r, ix->base_node);

        /* An owning array element is movable: reading it into an owning
           binding steals the element's pointer. The "[]" marker keeps
           element-moves distinct from whole-array moves in the moved-state
           map, while KeyRoot still reduces "base[]" to "base" for the
           global / ref-param ownership check. */
        return baseKey ? arena_format(r->m_arena, "%s[]", baseKey) : NULL;
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
        else if (otherVal == (void*)3 || dstVal == (void*)3)
        {
            StrMapPut(dst, other->keys[i], (void*)3);
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
static void ResolveExprImpl(Resolver* r, Node* n, StrMap* scope, bool asMemberBase);
static void ResolveExpr(Resolver* r, Node* n, StrMap* scope);
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope);

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range)
{
    Node* base = target;

    while (base->kind == NodeMember || base->kind == NodeIndex)
    {
        if (base->kind == NodeMember)
        {
            base = ((MemberExpr*)base)->base_node;
        }
        else
        {
            base = ((IndexExpr*)base)->base_node;
        }
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

/* Returns the result type of `copy(arg)`, or NULL if this isn't one.
   copy returns the same type as its argument (e.g. copy(string) → string). */
static const char* CopyBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0) return NULL;
    if (c->args.count != 1) return NULL;
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const char* t = InferType(r, arg0, scope);
    return (t && t[0]) ? arena_strdup(r->m_arena, t) : NULL;
}

static bool ResolveCopyBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0) return false;

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'copy' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const char* argType = InferType(r, arg0, scope);

    if (!argType || argType[0] == '\0' || !IsOwningType(argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range,
                     "'copy' expects an owning type (string, box<T>, T[]) — not '%s'", argType);
        return true;
    }

    c->isPseudoCall = true;
    return true;
}

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

/* Types the C ABI can carry through a bare extern `...` as a plain value:
   scalars, string (const char*), and handles. Structs, arrays, SIMD
   vectors, and void can't. */
static bool IsCVarargScalarish(Resolver* r, const char* type)
{
    if (IsNumeric(type) || strcmp(type, "string") == 0)
    {
        return true;
    }

    if (IsArrayType(type) || IsSimdVector(type))
    {
        return false;
    }

    if (TypeRegistryIsUserType(&r->m_registry, type))
    {
        return TypeRegistryIsOpaque(&r->m_registry, type);
    }

    return false;
}

/* Types that may be passed through a bare extern `...` (C varargs).
   Scalars, string, handles, and box<T> (which derefs to its value) reduce
   to something the C ABI can carry; box<T> is only allowed when T itself is
   scalar/string/handle, since a boxed struct would have to cross by value. */
static bool IsCVarargCompatible(Resolver* r, const char* type)
{
    if (strcmp(type, "void") == 0)
    {
        return false;
    }

    if (IsCVarargScalarish(r, type))
    {
        return true;
    }

    if (IsOwningType(type))
    {
        Str inner = OwningInnerStr(type);

        if (!inner.data)
        {
            return false;
        }

        const char* innerC = StrNew(r->m_arena, inner.data, inner.len).data;

        return IsCVarargScalarish(r, innerC);
    }

    return false;
}

/* Moves box/string sources into owned (non-ref) params, and into the
   collected array of a typed rest param when its element type is owning. */
static void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c)
{
    /* For a typed rest, the last param is the collected T[] array itself, so
       the arg at that slot is really the first ELEMENT. Only args before it
       map to named params; everything from the rest slot onward moves only if
       the ELEMENT type is owning (e.g. box<T>...), not because T[] is. */
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = typedRest && best->params.count > 0 ? best->params.count - 1 : best->params.count;

    for (size_t j = 0; j < c->args.count; j++)
    {
        Node* arg = (Node*)VecGet(&c->args, j);

        const char* targetType = NULL;
        bool moves = false;

        if (j < namedCount)
        {
            const ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);

            targetType = param->type.name;
            moves = IsOwningType(param->type.name) && param->mod == ModNone && !best->isExtern;
        }
        else if (typedRest)
        {
            const ParamDecl* restParam = (ParamDecl*)VecGet(&best->params, best->params.count - 1);
            Str inner = ArrayInnerStr(restParam->type.name);

            targetType = inner.data ? StrNew(r->m_arena, inner.data, inner.len).data : NULL;
            moves = targetType && IsOwningType(targetType) && restParam->mod == ModNone && !best->isExtern;
        }

        if (!moves || !targetType)
        {
            continue;
        }

        const char* movedArgKey = MovableBoxSourceKey(r, arg);

        if (movedArgKey)
        {
            MoveBoxIdent(r, movedArgKey, arg->range);
        }
    }
}

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        /* Constructor call FooBar(arg): an owning field initialized from an
           owning source is a move — enforce the same rules as var-decl and
           array_push (globals / ref params can't be moved). */
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            if (IsOwningType(fd->type.name))
            {
                const char* movedKey = MovableBoxSourceKey(r, arg);

                if (movedKey)
                {
                    MoveBoxIdent(r, movedKey, arg->range);
                }
            }
        }

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

    // copy(string/box<T>/T[]): deep-copy an owning value.
    if (c->callee != NULL && ResolveCopyBuiltin(r, c, scope))
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

        TrackCallArgMoves(r, best, c);

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

        if (functionDecl->isVariadic)
        {
            /* A bare extern `...` adds no declared param; a typed rest param
               is already one of params.count, so its minimal call is one
               fewer than that. */
            size_t minArgs = functionDecl->isCVararg ? functionDecl->params.count
                                                     : functionDecl->params.count - 1;

            if (c->args.count < minArgs)
            {
                continue;
            }
        }
        else if (functionDecl->params.count != c->args.count)
        {
            continue;
        }

        int score = 0;

        /* Variadic candidates are a fallback: an exact-arity non-variadic
           overload with the same name always wins when the call fits it. */
        if (functionDecl->isVariadic)
        {
            score += 1;
        }

        bool viable = true;

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);
            const char* argType = InferType(r, arg, scope);

            if (argType[0] == '\0')
            {
                continue;
            }

            /* Trailing args beyond the named params are collected by the
               variadic tail. A bare extern `...` adds no param; a typed rest
               param already counts as one, so its tail starts one earlier. */
            bool isTail = functionDecl->isVariadic
                && j >= (functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1);

            if (functionDecl->isCVararg && isTail)
            {
                /* Bare extern `...`: no declared element type. The arg must be
                   representable in a C vararg call. */
                if (!IsCVarargCompatible(r, argType))
                {
                    viable = false;
                    break;
                }

                /* Prefer exact-arity non-variadic overloads. */
                score += 2;
                continue;
            }

            const ParamDecl* param = (ParamDecl*)VecGet(
                &functionDecl->params, isTail ? functionDecl->params.count - 1 : j);

            const char* paramType = param->type.name;

            if (isTail)
            {
                /* Typed rest: compare against the element type (T of T[]). */
                Str inner = ArrayInnerStr(paramType);

                paramType = inner.data ? StrNew(r->m_arena, inner.data, inner.len).data : paramType;
            }

            if (strcmp(argType, paramType) == 0)
            {
            }
            else if (IsNumeric(argType) && IsNumeric(paramType))
            {
                score += 1;
            }
            else if (IsSimdVector(argType) && IsSimdVector(paramType))
            {
                score += 1;
            }
            else if (HandleExtendsFrom(&r->m_registry, argType, paramType))
            {
                score += 1;
            }
            else if (IsOwningType(argType))
            {
                /* box<T> coerces to T (implicit deref). */
                if (StrEqC(OwningInnerStr(argType), paramType))
                {
                    score += 1;
                }
                else
                {
                    viable = false;
                    break;
                }
            }
            else if (isTail && param->mod == ModNone && IsOwningType(paramType)
                     && StrEqC(OwningInnerStr(paramType), argType))
            {
                /* T coerces to box<T> (implicit boxing), matching array
                   literals. Only valid for owned (non-ref) typed-rest tail
                   args, where the collector boxes each element inline and the
                   callee owns it; a ref rest borrows, so it can't box. */
                score += 1;
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

    TrackCallArgMoves(r, best, c);
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

        builtinType = CopyBuiltinType(r, c, scope);

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
    ResolveExprImpl(r, n, scope, false);
}

static void ResolveExprImpl(Resolver* r, Node* n, StrMap* scope, bool asMemberBase)
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
        else if (IsOwningType(varType))
        {
            /* Whole-value use (asMemberBase=false): reject if fully OR
               partially moved. Member-base use (asMemberBase=true): reject
               only if fully moved — allows descending into a partially
               moved struct to access non-moved fields. */
            bool blocked = asMemberBase ? IsBoxMoved(r, ident->name)
                                        : IsBoxUnusable(r, ident->name);

            if (blocked)
            {
                if (IsBoxPartiallyMoved(r, ident->name))
                {
                    DiagErrorFmt(r->m_diag, ident->base.range,
                                 "'%s' is poisoned",
                                 ident->name);
                }
                else
                {
                    DiagErrorFmt(r->m_diag, ident->base.range, "'%s' used after move", ident->name);
                }
            }
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

        /* An owning struct field target (`bob.name = ...` where `name` is a
           string/box/array) is reassigned, not read - so it must re-life the
           field rather than trip the "used after move" check that a plain
           read of the target would. */
        const char* fieldKey = NULL;
        bool targetIsOwningField = false;

        if (a->target->kind == NodeMember)
        {
            const char* ft = InferType(r, a->target, scope);

            if (ft && ft[0] != '\0' && IsOwningType(ft))
            {
                targetIsOwningField = true;
                fieldKey = MovableBoxSourceKey(r, a->target);
            }
        }

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
                    DiagErrorFmt(r->m_diag, a->base.range, "'%s' used after move", targetName);
                }

                const char* inner = OwningInnerCStr(r->m_arena, tt);

                if (vt[0] != '\0' && !IsAssignableType(r, inner, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' into box<%s> '%s'", vt, inner,
                                 targetName);
                }
            }
        }
        else if (targetIsOwningField)
        {
            /* Writing to an owning field re-lives it. Only the root binding
               needs a move-check: a fully-moved root can't be written to, but
               a partially-moved root (or a partially-moved ancestor of a
               deeper field) is fine - one of its fields is being restored.
               Intermediate members are not read here, so their partially-
               moved state must not block the write. */
            Node* root = a->target;

            while (root->kind == NodeMember || root->kind == NodeIndex)
            {
                root = (root->kind == NodeMember) ? ((MemberExpr*)root)->base_node
                                                  : ((IndexExpr*)root)->base_node;
            }

            if (root->kind == NodeIdent)
            {
                ResolveExprImpl(r, root, scope, true);
            }

            if (a->op == AssignSet)
            {
                /* Plain `=` re-lives the field and any ancestor that no
                   longer has a moved descendant. */
                RevalidateOwningField(r, fieldKey);
            }
            else
            {
                /* Compound assign (+=, ...) reads the field first, so it's
                   only allowed while the field is still live. */
                ResolveExpr(r, a->target, scope);
            }

            ResolveExpr(r, a->value, scope);

            if (a->op == AssignSet)
            {
                /* If the RHS is itself an owning binding, it moves out of
                   that source - same as a box-to-box assignment. */
                const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                if (movedValueKey)
                {
                    MoveBoxIdent(r, movedValueKey, a->base.range);
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
        ResolveExprImpl(r, m->base_node, scope, true);

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

        /* A box-typed field can be moved out too - check the same way.
           IsBoxUnusable catches both fully-moved (exact field moved) and
           partially-moved (a descendant of this field was moved). */
        if (IsOwningType(InferType(r, n, scope)))
        {
            const char* key = MovableBoxSourceKey(r, n);

            if (key && IsBoxUnusable(r, key))
            {
                if (IsBoxPartiallyMoved(r, key))
                {
                    DiagErrorFmt(r->m_diag, m->base.range,
                                 "'%s' is poisoned",
                                 key);
                }
                else
                {
                    DiagErrorFmt(r->m_diag, m->base.range, "'%s' used after move", key);
                }
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

                /* A box<T> field moves its source — but only when the source
                   value itself is owning (box<T> into box<T>, string into
                   box<string>). A bare T boxed into a box<T> field is a copy. */
                const char* movedFieldKey
                    = (IsOwningType(fieldDecl->type.name)
                       && fieldValueType[0] != '\0' && IsOwningType(fieldValueType))
                      ? MovableBoxSourceKey(r, field->value) : NULL;

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
        ResolveExprImpl(r, ix->base_node, scope, true);
        ResolveExprImpl(r, ix->index, scope, false);
        return;
    }
    case NodeArrayInit:
    {
        ArrayInitExpr* ai = (ArrayInitExpr*)n;

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, i);
            ResolveExpr(r, elem, scope);

            /* An owning value (string/box) stored in an array literal moves
               its source, so mark it moved just like a var-decl move. */
            if (ai->elementType[0] != '\0' && IsOwningType(ai->elementType))
            {
                const char* movedKey = MovableBoxSourceKey(r, elem);

                if (movedKey)
                {
                    MoveBoxIdent(r, movedKey, elem->range);
                }
            }
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
           unrelated declaration in this function) — including field-level
           partial-move markers. */
        ClearBoxSubtree(r, vd->name);
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
