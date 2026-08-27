#include "Sema/ResolveOverloads.h"
#include "Codegen/TypeRegistry.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    Module* m_mod;
    DiagnosticEngine* m_diag;
    TypeRegistry m_registry;
    Arena* m_arena;
    StrMap m_constVars;
    StrMap m_movedBoxes;
    StrMap m_nonEmptyPaths; /* dotted path -> 1 = definitely non-empty (`T?` narrowing) */
    StrMap m_emptyPaths;    /* dotted path -> 1 = definitely EMPTY (else branch of `if (path?)`) */
    /* Index var name -> Vec of fact keys spelled with it ("i" -> {"arr[i].w"}).
       Mutating the var (or passing it non-const ref) drops those facts. */
    StrMap m_indexDeps;
    StrMap m_boxGlobals;
    StrMap m_refBoxParams;
    StrMap m_typeCache; /* canonical spelling -> interned TypeName tree */
    const TypeName* m_currentReturnType;
    Vec* m_liveLog; /* when set, MarkBoxLive records keys here (loop warmup) */
} Resolver;

/* Intern a TypeName tree per canonical spelling (shared by synthesized types). */
static const TypeName* InternTypeName(Resolver* r, const char* name)
{
    const TypeName* cached = (const TypeName*)StrMapGet(&r->m_typeCache, name);

    if (cached)
    {
        return cached;
    }

    TypeName* t = (TypeName*)arena_alloc(r->m_arena, sizeof(TypeName));
    *t = TypeNameParse(r->m_arena, name);
    StrMapPut(&r->m_typeCache, t->name, t);

    return t;
}

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

/* Resets a map's keys without freeing its buffer (recycles per-function state). */
static void ResetStrMap(StrMap* m)
{
    if (m->cap > 0)
    {
        memset(m->keys, 0, m->cap * sizeof(const char*));
        m->count = 0;
    }
}

/* Count of non-rest params for a call (excludes the trailing typed-rest param). */
static size_t NamedParamCount(const FunctionDecl* f)
{
    bool typedRest = f->isVariadic && !f->isCVararg;
    return typedRest && f->params.count > 0 ? f->params.count - 1 : f->params.count;
}

/* True if any fixed-size array dimension (`T[N]`) appears in the type tree (even inside a box). */
static bool TypeTreeHasFixedArray(const TypeName* t)
{
    for (; t; t = (t->isBox || t->isOptional) ? t->inner : (t->isArray ? t->elem : NULL))
    {
        if (t->isArray && t->length >= 0)
        {
            return true;
        }
    }

    return false;
}

/* Returns the innermost non-fixed node and the product of leading fixed dimensions (1 if none). */
static const TypeName* FixedArrayLeaf(const TypeName* t, long* totalCount)
{
    long count = 1;

    while (t && t->isArray && t->length >= 0)
    {
        count *= t->length;
        t = t->elem;
    }

    *totalCount = count;
    return t;
}

/* Strips a leading optional then box, returning the inner type (or NULL). */
static const TypeName* UnwrapBoxPtr(const TypeName* t)
{
    if (!t)
    {
        return NULL;
    }

    if (t->isOptional)
    {
        t = t->inner;
    }

    if (t && t->isBox)
    {
        t = t->inner;
    }

    return t;
}

/* Places fixed-array init leaves into `flat` at row-major offsets; short rows leave NULL holes. Returns false if an element lands beyond capacity. */
static bool PlaceFixedArrayInit(Vec* flat, size_t base, const TypeName* t, ArrayInitExpr* ai)
{
    const TypeName* rowType = t->elem;
    long innerTotal = 0;

    FixedArrayLeaf(rowType, &innerTotal);

    size_t slot = 0;

    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (slot >= (size_t)t->length)
        {
            return false;
        }

        if (rowType && rowType->isArray && rowType->length >= 0)
        {
            if (!PlaceFixedArrayInit(flat, base + slot * (size_t)innerTotal, rowType, (ArrayInitExpr*)elem))
            {
                return false;
            }
        }
        else
        {
            flat->items[base + slot] = elem;
        }

        slot++;
    }

    return true;
}

/* Verifies a fixed-array initializer's SHAPE: nested rows for multidimensional fields, a flat list for single-dimension. Returns false (with a diagnostic) on mismatch. */
static bool CheckFixedArrayInitShape(Resolver* r, const TypeName* t, ArrayInitExpr* ai, const TypeName* leaf,
                                     const char* fieldName)
{
    bool multidim = t->elem && t->elem->isArray && t->elem->length >= 0;
    bool leafIsStruct = leaf && TypeRegistryIsUserType(&r->m_registry, leaf->name)
                        && !TypeRegistryIsOpaque(&r->m_registry, leaf->name);

    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (multidim && elem->kind != NodeArrayInit)
        {
            DiagErrorFmt(r->m_diag, elem->range,
                         "multidimensional fixed-size array field '%s' requires nested rows "
                         "('{ { ... }, { ... } }'), not a flat element list",
                         fieldName);
            return false;
        }

        if (!multidim && elem->kind == NodeArrayInit && !leafIsStruct)
        {
            DiagErrorFmt(r->m_diag, elem->range,
                         "fixed-size array field '%s' has a single dimension - write its elements as a flat list",
                         fieldName);
            return false;
        }

        if (multidim && !CheckFixedArrayInitShape(r, t->elem, (ArrayInitExpr*)elem, leaf, fieldName))
        {
            return false;
        }
    }

    return true;
}

/* Move-state values: 1 = fully moved, 2 = re-live, 3 = partially moved. */
static bool IsBoxMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)1;
}

static void MarkBoxMoved(Resolver* r, const char* name)
{
    StrMapPut(&r->m_movedBoxes, name, (void*)1);
}

/* Whole-value use is rejected when fully (1) or partially (3) moved; non-moved fields stay accessible. */
static bool IsBoxUnusable(const Resolver* r, const char* name)
{
    void* v = StrMapGet(&r->m_movedBoxes, name);
    return v == (void*)1 || v == (void*)3;
}

/* A descendant field moved out, but the value itself wasn't: whole-value use rejected, fields still accessible. */
static bool IsBoxPartiallyMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)3;
}

/* Is `child` at or below `parent`? A '.' or '[' after the prefix continues the path; the erased "a[]" form covers every "a[...]" key. */
static bool PathIsDescendant(const char* parent, const char* child)
{
    if (strcmp(child, parent) == 0)
    {
        return true;
    }

    size_t plen = strlen(parent);

    if (strncmp(child, parent, plen) != 0)
    {
        return false;
    }

    char next = child[plen];

    if (next == '.' || next == '[')
    {
        return true;
    }

    /* "a[]" (erased) is the parent of every "a[...]" key. */
    if (plen >= 2 && parent[plen - 1] == ']' && parent[plen - 2] == '[')
    {
        return child[plen - 2] == '[';
    }

    return false;
}

/* Nulls every key in `m` that is `key` or a descendant of it. */
static void ClearSubtreeByPath(StrMap* m, const char* key)
{
    if (!key)
    {
        return;
    }

    for (size_t i = 0; i < m->cap; i++)
    {
        if (!m->keys[i])
        {
            continue;
        }

        if (PathIsDescendant(key, m->keys[i]))
        {
            m->values[i] = NULL;
        }
    }
}

/* Clears `key` and all its descendants from the move-state map.
   Used when a whole-value reassignment re-lives the binding. */
static void ClearBoxSubtree(Resolver* r, const char* key)
{
    ClearSubtreeByPath(&r->m_movedBoxes, key);
}

/* Nullable (`T?`) narrowing facts - defined below, used by move tracking. */
static void MarkPathNonEmpty(Resolver* r, const char* key);
static void ClearNonEmptySubtree(Resolver* r, const char* key);
static void ClearEmptySubtree(Resolver* r, const char* key);
static void ClearNullableFacts(Resolver* r, const char* key);

static void MarkBoxLive(Resolver* r, const char* name)
{
    ClearBoxSubtree(r, name);
    StrMapPut(&r->m_movedBoxes, name, (void*)2);

    if (r->m_liveLog)
    {
        VecPush(r->m_liveLog, (void*)name);
    }

    /* Reassignment re-lives the binding; clear stale nullable facts and re-establish non-empty. */
    ClearNullableFacts(r, name);
    MarkPathNonEmpty(r, name);
}

/* Reassigning one owning field re-lives it and any ancestor whose moved descendants are all restored. */
static void RevalidateOwningField(Resolver* r, const char* key)
{
    ClearBoxSubtree(r, key);
    /* Reassigning a single owning field re-installs a valid value there. */
    ClearNullableFacts(r, key);
    MarkPathNonEmpty(r, key);

    StrMap* m = &r->m_movedBoxes;

        /* Walk each dotted ancestor prefix of `key`. */
        for (const char* dot = strchr(key, '.'); dot; dot = strchr(dot + 1, '.'))
    {
        const char* prefix = arena_strndup(r->m_arena, key, (size_t)(dot - key));

            /* Only a partially-moved (3) ancestor can be restored. */
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

/* The binding a move key is rooted in ("holder.gun" -> "holder", "arr[3]" -> "arr") - used for global/ref-param checks. */
static const char* KeyRoot(Arena* arena, const char* key)
{
    const char* dot = strchr(key, '.');
    const char* base = dot ? arena_strndup(arena, key, (size_t)(dot - key)) : key;

    /* Drop the trailing bracket group (erased/var/literal). */
    const char* open = strrchr(base, '[');

    if (open)
    {
        return arena_strndup(arena, base, (size_t)(open - base));
    }

    return base;
}

/* Marks every proper path prefix of `key` as partially moved (3), unless already fully moved (1). Prefixes break at dots and brackets. */
static void MarkBoxPartiallyMoved(Resolver* r, const char* key)
{
    for (size_t i = 0; key[i]; i++)
    {
        if (key[i] != '.' && key[i] != '[')
        {
            continue;
        }

        if (i == 0)
        {
            continue;
        }

        char* prefix = arena_strndup(r->m_arena, key, i);

        if (StrMapGet(&r->m_movedBoxes, prefix) != (void*)1)
        {
            StrMapPut(&r->m_movedBoxes, prefix, (void*)3);
        }
    }
}

/* ---- Nullable (`T?`) "blessings" ----------------------------------------
   A path maps to 1 while provably non-empty. Reading an un-blessed optional is an error. Blessings die on any rebind/move/shadow, like move poisoning. */

static bool IsPathNonEmpty(const Resolver* r, const char* key);
static void MarkPathNonEmpty(Resolver* r, const char* key);
static void ClearNonEmptySubtree(Resolver* r, const char* key);

static bool IsPathNonEmpty(const Resolver* r, const char* key)
{
    return key && StrMapGet(&r->m_nonEmptyPaths, key) == (void*)1;
}

/* True when `key` contains the erased any-element form "a[]" - never provable. */
static bool PathKeyIsErased(const char* key)
{
    return key && strstr(key, "[]") != NULL;
}

static bool IsIdentStart(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool IsIdentCont(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Records each bracketed identifier in `key` as a dependency: mutating that local later drops the fact. */
static void TrackIndexDeps(Resolver* r, const char* key)
{
    for (const char* open = strchr(key, '['); open; open = strchr(open + 1, '['))
    {
        const char* close = strchr(open + 1, ']');

        if (!close)
        {
            continue;
        }

        for (const char* p = open + 1; p < close;)
        {
            if (!IsIdentStart(*p))
            {
                p++;
                continue;
            }

            const char* start = p;

            while (p < close && IsIdentCont(*p))
            {
                p++;
            }

            char* var = arena_strndup(r->m_arena, start, (size_t)(p - start));
            Vec* deps = (Vec*)StrMapGet(&r->m_indexDeps, var);

            if (!deps)
            {
                deps = (Vec*)arena_alloc(r->m_arena, sizeof(Vec));
                VecInit(deps);
                StrMapPut(&r->m_indexDeps, var, deps);
            }

            VecPush(deps, (void*)key);
        }
    }
}

static void MarkPathNonEmpty(Resolver* r, const char* key)
{
    if (!key || PathKeyIsErased(key))
    {
        return;
    }

    StrMapPut(&r->m_nonEmptyPaths, key, (void*)1);
    TrackIndexDeps(r, key);
}

/* Drops every fact spelled with `[var]`; called when `var` is assigned, incremented, or passed non-const ref. */
static void InvalidateIndexVar(Resolver* r, const char* var)
{
    if (!var)
    {
        return;
    }

    Vec* deps = (Vec*)StrMapGet(&r->m_indexDeps, var);

    if (!deps)
    {
        return;
    }

    for (size_t i = 0; i < deps->count; i++)
    {
        const char* key = (const char*)VecGet(deps, i);

        ClearNullableFacts(r, key);
    }

    StrMapPut(&r->m_indexDeps, var, NULL);
}

/* Drops `key` and all `key.*` descendants from the non-empty map. */
static void ClearNonEmptySubtree(Resolver* r, const char* key)
{
    ClearSubtreeByPath(&r->m_nonEmptyPaths, key);
}

static void ClearAllNonEmptyFacts(Resolver* r)
{
    StrMap* m = &r->m_nonEmptyPaths;

    for (size_t i = 0; i < m->cap; i++)
    {
        m->values[i] = NULL;
    }

    StrMap* e = &r->m_emptyPaths;

    for (size_t i = 0; i < e->cap; i++)
    {
        e->values[i] = NULL;
    }
}

/* ---- Definitely-EMPTY facts ---------------------------------------------
   The else branch of `if (path?)` proves the path empty. Never leaves its block; dropped by any rebind. Sharper diagnostics than a merely-unproven path. */

static bool IsPathDefinitelyEmpty(const Resolver* r, const char* key)
{
    return key && StrMapGet(&r->m_emptyPaths, key) == (void*)1;
}

static void MarkPathEmpty(Resolver* r, const char* key)
{
    if (!key || PathKeyIsErased(key))
    {
        return;
    }

    /* A path cannot be both. */
    ClearNonEmptySubtree(r, key);
    StrMapPut(&r->m_emptyPaths, key, (void*)1);
    TrackIndexDeps(r, key);
}

/* Drops `key` and all its descendants from the EMPTY map. */
static void ClearEmptySubtree(Resolver* r, const char* key)
{
    ClearSubtreeByPath(&r->m_emptyPaths, key);
}

/* Drops both nullable narrowing facts (non-empty and empty) for `key`. */
static void ClearNullableFacts(Resolver* r, const char* key)
{
    ClearNonEmptySubtree(r, key);
    ClearEmptySubtree(r, key);
}


/* "is definitely empty" for else-proven paths, else "has not been blessed". */
static const char* EmptyWording(const Resolver* r, const char* key)
{
    return IsPathDefinitelyEmpty(r, key) ? "is definitely empty" : "has not been blessed";
}

static const char* MovableBoxSourceKey(Resolver* r, Node* n);

/* Diagnostic for reading an un-blessed optional. An erased key ("arr[]") can never be blessed - suggest materializing into a local. */
static void DiagOptionalReadError(Resolver* r, SourceRange range, const char* key, const char* typeName)
{
    if (PathKeyIsErased(key))
    {
        size_t nlen = strlen(typeName);
        bool alreadyOptional = nlen > 0 && typeName[nlen - 1] == '?';

        DiagErrorFmt(r->m_diag, range,
                     "'%s' has not been blessed ('%s'); the array index is not a constant or a trackable variable - "
                     "move or copy the element into a local first ('%s x = %s...; if (x?) ...')",
                     key, typeName, alreadyOptional ? typeName : "T?", key);
        return;
    }

    DiagErrorFmt(r->m_diag, range,
                 "'%s' %s ('%s'); test it first: if (%s?) { ... }",
                 key ? key : "<expression>", EmptyWording(r, key), typeName,
                 key ? key : "<expr>");
}

/* Reading optional CONTENTS into a non-optional target needs a narrowing fact. Box-shaped targets (`T?`/`^T`) rebind, so exempt. */
static void CheckOptionalDeref(Resolver* r, Node* value, const TypeName* valueType, const TypeName* targetType,
                               SourceRange range)
{
    if (!valueType || !valueType->isOptional)
    {
        return;
    }

    if (!targetType || targetType->isOptional || targetType->isBox)
    {
        return;
    }

    const char* key = MovableBoxSourceKey(r, value);

    if (IsPathNonEmpty(r, key))
    {
        return;
    }

    DiagOptionalReadError(r, range, key, valueType->name);
}

/* Extracts the operand of `path?`/`!path?` (any `!` count); `*negated` is true when the condition asserts EMPTY. */
static Node* CondNullTestOperand(Node* cond, bool* negated)
{
    bool neg = false;
    Node* inner = cond;

    while (inner && inner->kind == NodeUnary && ((UnaryExpr*)inner)->op == UnNot)
    {
        inner = ((UnaryExpr*)inner)->operand;
        neg = !neg;
    }

    *negated = neg;

    if (inner && inner->kind == NodeNullTest)
    {
        return ((NullTestExpr*)inner)->operand;
    }

    return NULL;
}

/* Intersect fact sets: a path stays non-empty only if both branches prove it. */
static void MergeNonEmptyFacts(StrMap* dst, const StrMap* other)
{
    for (size_t i = 0; i < dst->cap; i++)
    {
        if (!dst->keys[i] || dst->values[i] != (void*)1)
        {
            continue;
        }

        if (StrMapGet(other, dst->keys[i]) != (void*)1)
        {
            dst->values[i] = NULL;
        }
    }

    /* Keys that exist only in `other` cannot survive the merge either. */
}

/* Marks `name` moved, unless it's a box global or `ref ^T` param (borrowed, not owned). */
static void MoveBoxIdent(Resolver* r, const char* name, SourceRange range)
{
    const char* root = KeyRoot(r->m_arena, name);

    if (IsBoxGlobalName(r, root))
    {
        DiagErrorFmt(r->m_diag, range,
                      "'%s' cannot be moved as it is not owned because it is global. (use a `[const] ref ^T`, copy() it, or pass `T` by value)",
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
    /* Moving out also clears the path's non-empty fact. */
    ClearNonEmptySubtree(r, name);
}

/* Moving a `T?` leaves the source EMPTY (legal), not moved. No poison; later `path?` reads false. */
static void MoveOptionalSource(Resolver* r, const char* name, SourceRange range)
{
    (void)range;

    ClearNullableFacts(r, name);
}

/* True if `name` is a module global - untracked as a precise index spelling. */
static bool IsModuleGlobalName(const Resolver* r, const char* name)
{
    for (size_t i = 0; i < r->m_mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->m_mod->globals, i);

        if (strcmp(gd->name, name) == 0)
        {
            return true;
        }
    }

    return false;
}

/* Stable index spelling for path keys: literals, locals, `.length`, unary, and integer binaries (fully parenthesized). Returns NULL when unpinnable - caller erases the key (conservative for moves). */
static char* IndexExprSpelling(Resolver* r, Node* n)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        return arena_format(r->m_arena, "%llu", ((IntLiteral*)n)->value);

    case NodeIdent:
    {
        const char* name = ((IdentExpr*)n)->name;

        /* A global has no observable mutation site: erase. */
        return IsModuleGlobalName(r, name) ? NULL : arena_format(r->m_arena, "%s", name);
    }

    case NodeMember:
    {
        /* `.length` of a bare local only. */
        MemberExpr* m = (MemberExpr*)n;

        if (strcmp(m->member, "length") == 0 && m->base_node->kind == NodeIdent
            && !IsModuleGlobalName(r, ((IdentExpr*)m->base_node)->name))
        {
            return arena_format(r->m_arena, "%s.length", ((IdentExpr*)m->base_node)->name);
        }

        return NULL;
    }

    case NodeIndex:
    {
        /* Nested index: spell base and bracket recursively. */
        IndexExpr* ix = (IndexExpr*)n;
        char* baseSpell = IndexExprSpelling(r, ix->base_node);
        char* inner = IndexExprSpelling(r, ix->index);

        if (!baseSpell || !inner)
        {
            return NULL;
        }

        return arena_format(r->m_arena, "%s[%s]", baseSpell, inner);
    }

    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;
        char* inner = IndexExprSpelling(r, u->operand);

        if (!inner)
        {
            return NULL;
        }

        switch (u->op)
        {
        case UnNeg:
            return arena_format(r->m_arena, "-%s", inner);
        case UnPos:
            return arena_format(r->m_arena, "+%s", inner);
        case UnBitNot:
            return arena_format(r->m_arena, "~%s", inner);
        default:
            return NULL; /* UnNot: a boolean is not an index */
        }
    }

    case NodeBinary:
    {
        BinaryExpr* bx = (BinaryExpr*)n;
        char* lhs = IndexExprSpelling(r, bx->lhs);
        char* rhs = IndexExprSpelling(r, bx->rhs);

        if (!lhs || !rhs)
        {
            return NULL;
        }

        const char* op = NULL;

        switch (bx->op)
        {
        case BinAdd:
            op = "+";
            break;
        case BinSub:
            op = "-";
            break;
        case BinMul:
            op = "*";
            break;
        case BinDiv:
            op = "/";
            break;
        case BinMod:
            op = "%";
            break;
        case BinBitAnd:
            op = "&";
            break;
        case BinBitOr:
            op = "|";
            break;
        case BinBitXor:
            op = "^";
            break;
        case BinShl:
            op = "<<";
            break;
        case BinShr:
            op = ">>";
            break;
        default:
            return NULL; /* comparisons/logic yield bools, not indices */
        }

        return arena_format(r->m_arena, "(%s %s %s)", lhs, op, rhs);
    }

    default:
        return NULL;
    }
}

/* Move-tracking key for `n` (casts unwrapped); NULL if not movable. */
static const char* MovableBoxSourceKey(Resolver* r, Node* n)
{
    /* Shared "what can be moved" rule (unwrap casts; ident/member/index). */
    const Node* moved = MovableBoxSourceNode(n);

    if (!moved)
    {
        return NULL;
    }

    if (moved->kind == NodeIdent)
    {
        return ((const IdentExpr*)moved)->name;
    }

    if (moved->kind == NodeMember)
    {
        const MemberExpr* m = (const MemberExpr*)moved;
        const char* baseKey = MovableBoxSourceKey(r, m->base_node);

        return baseKey ? arena_format(r->m_arena, "%s.%s", baseKey, m->member) : NULL;
    }

    if (moved->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)moved;
        const char* baseKey = MovableBoxSourceKey(r, ix->base_node);

        if (!baseKey)
        {
            return NULL;
        }

        /* Spell the index precisely (literal/local/arithmetic) so distinct elements are distinct keys; unpinnable spellings erase to "a[]" (conservative, never provable). "[]" also separates element moves from whole-array moves. */
        char* idxSpell = IndexExprSpelling(r, ix->index);

        if (idxSpell)
        {
            return arena_format(r->m_arena, "%s[%s]", baseKey, idxSpell);
        }

        return arena_format(r->m_arena, "%s[]", baseKey);
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

/* Conservative branch merge: a name is moved if either side moved it, else live if either side re-lived it. */
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

static const TypeName* InferType(Resolver* r, Node* n, StrMap* scope);

static void WalkBlock(Resolver* r, Block* b, StrMap* scope);
static void WalkStmt(Resolver* r, Node* n, StrMap* scope);
static void ResolveExprImpl(Resolver* r, Node* n, StrMap* scope, bool asMemberBase);
static void ResolveExpr(Resolver* r, Node* n, StrMap* scope);
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope, const char* condFactKey, bool condFactNegated);
static void CheckCallArgOptionalDerefs(Resolver* r, CallExpr* c, StrMap* scope);

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range)
{
    Node* base = target;

    if (!base)
    {
        return;
    }

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

/* Resolves SIMD vector constructors (`float3(x)`, `float4(w,x,y,z)`); returns true if this was one. */
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

        const TypeName* argType = InferType(r, arg, scope);

        if (!argType)
        {
            continue;
        }

        if (strcmp(argType->name, "float") != 0)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "expected argument of type 'float' but found '%s'", argType->name);
            return false;
        }
    }

    c->isPseudoCall = true;

    return true;
}

//-- Array intrinsics.

/* Result type of `copy(arg)`, or NULL if not a copy call. */
static const TypeName* CopyBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0) return NULL;
    if (c->args.count != 1) return NULL;
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    return InferType(r, arg0, scope);
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
    const TypeName* argType = InferType(r, arg0, scope);

    if (!argType || !TypeNameIsOwning(argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range,
                     "'copy' expects an owning type (string, ^T, T[]) — not '%s'", argType ? argType->name : "");
        return true;
    }

    c->isPseudoCall = true;
    return true;
}

/* Result type of array_push/pop/resize, or NULL if not one. */
static const TypeName* ArrayBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
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
    const TypeName* arrType = arg0 ? InferType(r, arg0, scope) : NULL;

    /* arg0 must be an array for this to be a (valid) builtin call. */
    const TypeName* elem = arrType ? TypeNameArrayElem(arrType) : NULL;

    if (!elem)
    {
        return NULL;
    }

    if (isPop)
    {
        return elem;
    }

    return InternTypeName(r, isPush ? "ulong" : "void");
}

static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType);

/* True if `n` (casts unwrapped) is an array-element read `arr[i]` - a borrow whose owner retains the value. */
static bool IsArrayElementBorrow(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return n && n->kind == NodeIndex;
}

/* Validates an array builtin and marks it pseudo; returns true if it's a push/pop/resize (valid or not). */
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

    const TypeName* arrType = InferType(r, arg0, scope);

    /* A `T[]?` receiver derefs like an optional read; unwrap for element type. */
    const TypeName* unwrapped = arrType && arrType->isOptional ? arrType->inner : arrType;

    if (!TypeNameIsDynamicArray(unwrapped))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' expects an array argument, not '%s'", c->callee,
                     arrType ? arrType->name : "");
        return true;
    }

    /* The array is mutated in place, so it must be addressable (an lvalue). */
    if (arg0->kind != NodeIdent && arg0->kind != NodeMember && arg0->kind != NodeIndex)
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' array argument must be an lvalue", c->callee);
        return true;
    }

    /* Length may change: drop facts spelled through `[recv.length ...]`. Push keeps element facts; resize/pop drop them below. */
    {
        const char* lenRecvKey = MovableBoxSourceKey(r, arg0);

        if (lenRecvKey)
        {
            InvalidateIndexVar(r, KeyRoot(r->m_arena, lenRecvKey));
        }
    }

    /* resize/pop empty slots we track - drop all element facts. push preserves them. */
    if (isResize || isPop)
    {
        const char* recvKey = MovableBoxSourceKey(r, arg0);

        if (recvKey)
        {
            ClearNullableFacts(r, recvKey);
        }
    }

    if (isResize)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);

        const TypeName* sizeType = InferType(r, arg1, scope);

        if (sizeType && !IsNumeric(sizeType->name))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "'array_resize' size must be an integer, not '%s'", sizeType->name);
        }
    }
    else if (isPush)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);

        const TypeName* valueType = InferType(r, arg1, scope);
        const TypeName* elemType = TypeNameArrayElem(unwrapped);

        if (valueType && elemType && !IsAssignableType(r, elemType, valueType))
        {
            /* A blessed `T?` pushed into `^T[]` may be moved (source left empty). */
            const TypeName* vi = TypeNameBoxInner(valueType);
            const TypeName* ei = TypeNameBoxInner(elemType);

            bool narrowedOptIntoBox = valueType->isOptional && elemType->isBox && vi && ei
                                      && strcmp(vi->name, ei->name) == 0
                                      && IsPathNonEmpty(r, MovableBoxSourceKey(r, arg1));

            if (!narrowedOptIntoBox)
            {
                DiagErrorFmt(r->m_diag, arg1->range,
                             "cannot push a value of type '%s' into '%s' (element type '%s')",
                             valueType->name, arrType->name, elemType->name);
            }
        }

        /* Pushing a borrow out of an array element would duplicate ownership; move it to a local first. */
        if (valueType && TypeNameIsOwning(valueType) && IsArrayElementBorrow(arg1))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push '%s' read from an array element - it would be owned by two arrays; "
                         "move it into a variable first",
                         valueType->name);
        }

        /* Deref check before the move tracking below clears the pushed optional's fact. */
        CheckCallArgOptionalDerefs(r, c, scope);

        /* Pushing an owning value moves it in; an optional source is left EMPTY, never poisoning its parent. */
        if (valueType && TypeNameIsOwning(valueType))
        {
            const char* movedKey = MovableBoxSourceKey(r, arg1);

            if (movedKey)
            {
                if (valueType->isOptional)
                {
                    MoveOptionalSource(r, movedKey, c->base.range);
                }
                else
                {
                    MoveBoxIdent(r, movedKey, c->base.range);
                }
            }
        }
    }

    c->isPseudoCall = true;

    return true;
}
//--

/* Types a bare extern `...` can carry: scalars, string, handles. Not structs/arrays/SIMD/void. */
static bool IsCVarargScalarish(Resolver* r, const TypeName* type)
{
    if (!type)
    {
        return false;
    }

    if (IsNumeric(type->name) || strcmp(type->name, "string") == 0)
    {
        return true;
    }

    if (type->isArray || IsSimdVector(type->name))
    {
        return false;
    }

    if (TypeRegistryIsUserType(&r->m_registry, type->name))
    {
        return TypeRegistryIsOpaque(&r->m_registry, type->name);
    }

    return false;
}

/* Types allowed through extern `...`: scalars/string/handles, and `^T` only when T is scalar/string/handle. */
static bool IsCVarargCompatible(Resolver* r, const TypeName* type)
{
    if (!type || strcmp(type->name, "void") == 0)
    {
        return false;
    }

    if (IsCVarargScalarish(r, type))
    {
        return true;
    }

    if (TypeNameIsOwning(type))
    {
        return IsCVarargScalarish(r, TypeNameBoxInner(type));
    }

    return false;
}

/* Moves box/string sources into owned params and rest-collected arrays. Also invalidates narrowing facts a real call could break (non-const ref mutation, global rebinds). */
static void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c)
{
    /* A real call may rebind globals - global-rooted facts never survive; locals are unaffected. */
    for (size_t g = 0; g < r->m_mod->globals.count; g++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->m_mod->globals, g);

        ClearNullableFacts(r, gd->name);
    }

    /* With a typed rest, the rest slot holds the first ELEMENT; later args move only if the element type owns. */
        bool typedRest = best->isVariadic && !best->isCVararg;
        size_t namedCount = NamedParamCount(best);

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);

            /* A non-const `ref` arg may be mutated by the callee - drop its facts and index use. */
        if (j < namedCount)
        {
            const ParamDecl* rp = (ParamDecl*)VecGet(&best->params, j);

            if (rp->mod == ModRef && !rp->type.isConst)
            {
                if (arg->kind == NodeIdent)
                {
                    InvalidateIndexVar(r, ((IdentExpr*)arg)->name);
                }

                const char* refKey = MovableBoxSourceKey(r, arg);

                if (refKey)
                {
                    /* Callee may rebind/empty through the ref and write array elements - drop nested facts. */
                    InvalidateIndexVar(r, KeyRoot(r->m_arena, refKey));
                    ClearNullableFacts(r, refKey);
                }
            }
        }
        else if (typedRest && best->params.count > 0)
        {
            const ParamDecl* rp = (ParamDecl*)VecGet(&best->params, best->params.count - 1);

            if (rp->mod == ModRef && !rp->type.isConst && arg->kind == NodeIdent)
            {
                InvalidateIndexVar(r, ((IdentExpr*)arg)->name);
            }
        }

        const TypeName* targetType = NULL;
        bool moves = false;

        if (j < namedCount)
        {
            const ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);

            targetType = &param->type;
            moves = TypeNameIsOwning(&param->type) && param->mod == ModNone && !best->isExtern;
        }
        else if (typedRest)
        {
            const ParamDecl* restParam = (ParamDecl*)VecGet(&best->params, best->params.count - 1);
            const TypeName* elem = TypeNameArrayElem(&restParam->type);

            targetType = elem;
            moves = targetType && TypeNameIsOwning(targetType) && restParam->mod == ModNone && !best->isExtern;
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

/* Every `T?` arg whose target wants the CONTENTS (T param, rest element, struct field, array_push element, or extern `...`) must be proven non-empty. */
static void CheckCallArgOptionalDerefs(Resolver* r, CallExpr* c, StrMap* scope)
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
                /* Extern `...`: the inner type is the effective target. */
                target = TypeNameBoxInner(at);
            }

            CheckOptionalDeref(r, arg, at, target, arg->range);
        }

        return;
    }

    /* Struct constructor FooBar(opt): the field slots are the targets. */
    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd2 = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            CheckOptionalDeref(r, arg, InferType(r, arg, scope), &fd2->type, arg->range);
        }

        return;
    }

    /* array_push: the element slot is the target; a `T[]?` receiver is itself dereferenced. */
    if (c->callee && strcmp(c->callee, "array_push") == 0 && c->args.count == 2)
    {
        const TypeName* arrType = InferType(r, (Node*)VecGet(&c->args, 0), scope);
        const TypeName* elem = arrType && arrType->isOptional ? TypeNameArrayElem(arrType->inner)
                                                              : TypeNameArrayElem(arrType);
        Node* valArg = (Node*)VecGet(&c->args, 1);

        CheckOptionalDeref(r, valArg, InferType(r, valArg, scope), elem, valArg->range);

        if (arrType && arrType->isOptional)
        {
            Node* arrArg = (Node*)VecGet(&c->args, 0);

            CheckOptionalDeref(r, arrArg, arrType, arrType->inner, arrArg->range);
        }
    }
}

/* Rewrites a braced list into a positional StructInitExpr of `structName` (parser can't tell struct vs array init). */
static Node* BracedToStructInit(Resolver* r, Node* braced, const char* structName)
{
    ArrayInitExpr* ai = AsNode(ArrayInitExpr, braced);

    StructInitExpr* init = (StructInitExpr*)arena_alloc(r->m_arena, sizeof(StructInitExpr));
    memset(init, 0, sizeof(StructInitExpr));
    init->base.kind = NodeStructInit;
    init->base.range = ai->base.range;
    init->typeName = (char*)structName;
    VecInit(&init->fields);

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        StructInitField* field = (StructInitField*)arena_alloc(r->m_arena, sizeof(StructInitField));
        field->name = NULL;
        field->value = (Node*)VecGet(&ai->elements, i);
        VecPush(&init->fields, field);
    }

    return (Node*)init;
}

/* Applies a struct target type to a context-free braced node, or fills a designator's typeName. Unchanged if target isn't a defined struct. */
static Node* ApplyBracedStructTarget(Resolver* r, Node* node, const TypeName* target)
{
    const TypeName* unwrapped = UnwrapBoxPtr(target);

    if (!unwrapped || !TypeRegistryIsUserType(&r->m_registry, unwrapped->name)
        || TypeRegistryIsOpaque(&r->m_registry, unwrapped->name))
    {
        return node;
    }

    if (node->kind == NodeArrayInit && !((ArrayInitExpr*)node)->elementType)
    {
        return BracedToStructInit(r, node, unwrapped->name);
    }

    if (node->kind == NodeStructInit && !((StructInitExpr*)node)->typeName)
    {
        ((StructInitExpr*)node)->typeName = (char*)unwrapped->name;
    }

    return node;
}

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        /* Struct ctor: an owning field from an owning source is a move (same rules as var-decl). Deref checks run before moves so the fact at call time is used. */
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            /* A braced arg against a struct field is a positional struct init; against an array field, an array literal. */
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

            if (resolvedArg != arg)
            {
                VecSet(&c->args, j, resolvedArg);
            }
            else if (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
            {
                /* Array field from a braced list: infer element type and redo move-marking (skipped when first resolved with NULL type). */
                const TypeName* ft = &fd->type;
                const TypeName* ftArr = ft->isOptional ? ft->inner : ft;

                if (TypeNameIsDynamicArray(ftArr))
                {
                    const TypeName* elem = TypeNameArrayElem(ftArr);
                    ArrayInitExpr* ai = (ArrayInitExpr*)arg;

                    ai->elementType = elem;

                    if (elem && TypeNameIsOwning(elem))
                    {
                        for (size_t k = 0; k < ai->elements.count; k++)
                        {
                            Node* elemNode = (Node*)VecGet(&ai->elements, k);
                            const char* movedKey = MovableBoxSourceKey(r, elemNode);

                            if (movedKey)
                            {
                                MoveBoxIdent(r, movedKey, elemNode->range);
                            }
                        }
                    }
                }
            }
        }

        /* Facts before moves: a move clears the fact, but the call-time fact must be used. */
        CheckCallArgOptionalDerefs(r, c, scope);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            if (TypeNameIsOwning(&fd->type))
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

    // copy(string/^T/T[]): deep-copy an owning value.
    if (c->callee != NULL && ResolveCopyBuiltin(r, c, scope))
    {
        return;
    }

    /* Call already resolved (e.g. warmup); reuse cached decl and rerun move tracking. */
    if (c->resolvedDecl)
    {
        const FunctionDecl* best = c->resolvedDecl;

        /* Facts before moves. */
        CheckCallArgOptionalDerefs(r, c, scope);
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
            /* Extern `...` adds no param; a typed rest counts as one, so min args is one fewer. */
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

        /* Variadic candidates are a lower-priority fallback. */
        if (functionDecl->isVariadic)
        {
            score += 1;
        }

        bool viable = true;

        for (size_t j = 0; j < c->args.count; j++)
        {
            Node* arg = (Node*)VecGet(&c->args, j);
            const TypeName* argType = InferType(r, arg, scope);

            if (!argType)
            {
                continue;
            }

            /* Trailing args past named params go to the variadic tail; rest starts one earlier than its count. */
            bool isTail = functionDecl->isVariadic
                && j >= (functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1);

            if (functionDecl->isCVararg && isTail)
            {
                /* Extern `...`: arg must be C-vararg representable. */
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

            const TypeName* paramType = &param->type;

            if (isTail)
            {
                /* Typed rest: compare against the element type (T of T[]). */
                const TypeName* elem = TypeNameArrayElem(paramType);

                if (elem)
                {
                    paramType = elem;
                }
            }

            if (strcmp(argType->name, paramType->name) == 0)
            {
            }
            else if (IsNumeric(argType->name) && IsNumeric(paramType->name))
            {
                score += 1;
            }
            else if (IsSimdVector(argType->name) && IsSimdVector(paramType->name))
            {
                score += 1;
            }
            else if (HandleExtendsFrom(&r->m_registry, argType->name, paramType->name))
            {
                score += 1;
            }
            else if (paramType->isOptional)
            {
                /* `Weapon` and `^Weapon` coerce into `Weapon?` when inners match. */
                const char* paramInner = TypeNameBoxInner(paramType) ? TypeNameBoxInner(paramType)->name : NULL;

                bool optMatch = false;

                if (paramInner && (argType->isBox || argType->isOptional))
                {
                    const TypeName* argInner = TypeNameBoxInner(argType);

                    optMatch = argInner != NULL
                               ? strcmp(argInner->name, paramInner) == 0
                               : strcmp("string", paramInner) == 0; /* bare ^string */
                }
                else if (paramInner)
                {
                    optMatch = strcmp(argType->name, paramInner) == 0;
                }

                if (optMatch)
                {
                    score += 1;
                }
                else
                {
                    viable = false;
                    break;
                }
            }
            else if (TypeNameIsOwning(argType))
            {
                /* ^T coerces to T (implicit deref). */
                const TypeName* argInner = TypeNameBoxInner(argType);

                if (argInner && strcmp(argInner->name, paramType->name) == 0)
                {
                    score += 1;
                }
                else
                {
                    viable = false;
                    break;
                }
            }
            else if (isTail && param->mod == ModNone && TypeNameIsOwning(paramType))
            {
                const TypeName* paramInner = TypeNameBoxInner(paramType);

                /* T coerces to ^T on an owned typed-rest tail (collector boxes inline); a ref rest can't box. */
                if (paramInner && strcmp(paramInner->name, argType->name) == 0)
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

    /* Braced arg against a struct param is a positional struct init; rewrite before checks. */
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

            const ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &param->type);

            if (resolvedArg != arg)
            {
                VecSet(&c->args, j, resolvedArg);
            }
        }
    }

    CheckCallArgOptionalDerefs(r, c, scope);
    TrackCallArgMoves(r, best, c);
}

static const TypeName* InferType(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    {
        const IntLiteral* lit = (const IntLiteral*)n;

        if (lit->value > 0xFFFFFFFFULL)
        {
            return InternTypeName(r, lit->isUnsigned ? "ulong" : "long");
        }

        return InternTypeName(r, lit->isUnsigned ? "uint" : "int");
    }
    case NodeFloatLiteral:
        return InternTypeName(r, "float");
    case NodeBoolLiteral:
        return InternTypeName(r, "bool");
    case NodeStrLiteral:
        return InternTypeName(r, "string");
    case NodeIdent:
    {
        const TypeName* t = (const TypeName*)StrMapGet(scope, ((IdentExpr*)n)->name);

        return t ? t : NULL;
    }
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;

        return u->op == UnNot ? InternTypeName(r, "bool") : InferType(r, u->operand, scope);
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
            return InternTypeName(r, "bool");
        default:
        {
            const TypeName* lt = InferType(r, b->lhs, scope);
            const TypeName* rt = InferType(r, b->rhs, scope);
            const char* ln = lt ? lt->name : "";
            const char* rn = rt ? rt->name : "";

            if (strcmp(ln, "double") == 0 || strcmp(rn, "double") == 0)
            {
                return InternTypeName(r, "double");
            }

            if (strcmp(ln, "float") == 0 || strcmp(rn, "float") == 0)
            {
                return InternTypeName(r, "float");
            }

            if (strcmp(ln, "ulong") == 0 || strcmp(rn, "ulong") == 0)
            {
                return InternTypeName(r, "ulong");
            }

            if (strcmp(ln, "long") == 0 || strcmp(rn, "long") == 0)
            {
                return InternTypeName(r, "long");
            }

            if (strcmp(ln, "uint") == 0 || strcmp(rn, "uint") == 0)
            {
                return InternTypeName(r, "uint");
            }

            return InternTypeName(r, "int");
        }
        }
    }
    case NodeAssign:
        return InferType(r, ((AssignExpr*)n)->target, scope);
    case NodeNullTest:
        return InternTypeName(r, "bool");
    case NodeIncDec:
        return InferType(r, ((IncDecExpr*)n)->operand, scope);
    case NodeCast:
        return &((CastExpr*)n)->type;
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;

        const TypeName* baseType = InferType(r, m->base_node, scope);

        /* Optional member access follows the box pointer. */
        baseType = UnwrapBoxPtr(baseType);

        if (!baseType)
        {
            return NULL;
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseType->name))
        {
            if (IsIncompleteStruct(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of incomplete type '%s'", m->member,
                             baseType->name);
            }
            else
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of opaque handle '%s'", m->member,
                             baseType->name);
            }

            return NULL;
        }

        const StructType* structType = TypeRegistryFind(&r->m_registry, baseType->name);
        if (!structType)
        {
            return NULL;
        }

        int idx = TypeRegistryFieldIndex(&r->m_registry, baseType->name, m->member);
        if (idx < 0)
        {
            return NULL;
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet((Vec*)&structType->fields, (size_t)idx);

        return &fieldDecl->type;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;

        if (c->resolvedDecl)
        {
            return &c->resolvedDecl->returnType;
        }

        const TypeName* builtinType = ArrayBuiltinType(r, c, scope);

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
            return InternTypeName(r, c->callee);
        }

        return NULL;
    }
    case NodeStructInit:
        return ((StructInitExpr*)n)->typeName ? InternTypeName(r, ((StructInitExpr*)n)->typeName) : NULL;
    case NodeIndex:
    {
        const TypeName* baseType = InferType(r, ((IndexExpr*)n)->base_node, scope);

        /* Indexing through an optional array (`T[]?`) unwraps it. */
        if (baseType && baseType->isOptional)
        {
            baseType = baseType->inner;
        }

        return baseType ? TypeNameArrayElem(baseType) : NULL;
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* ai = (const ArrayInitExpr*)n;

        return ai->elementType ? InternTypeName(r, arena_format(r->m_arena, "%s[]", ai->elementType->name)) : NULL;
    }
    default:
        return NULL;
    }
}

static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType)
{
    if (targetType && TypeNameIsOwning(targetType))
    {
        const TypeName* inner = TypeNameBoxInner(targetType);

        /* `T?` accepts `^T` (and vice versa) when inners match (same ABI). The reverse needs a narrowing fact (checked by callers). */
        const TypeName* valueInner = TypeNameBoxInner(valueType);

        if (targetType->isOptional && valueType->isBox && inner && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        return (inner && strcmp(inner->name, valueType->name) == 0)
               || strcmp(valueType->name, targetType->name) == 0;
    }

    // Assignable if type is exact match
    if (strcmp(valueType->name, targetType->name) == 0)
    {
        return true;
    }

    // If both types are numeric (and therefore convertible)
    // TODO: Require explicit casts when performing lossy conversions (e.g. ulong to uint)
    if (IsNumeric(valueType->name) && IsNumeric(targetType->name))
    {
        return true;
    }

    // Assignment to a SIMD vector (vector = scalar, vector = vector)
    if (IsSimdVector(targetType->name) && (IsSimdVector(valueType->name) || IsNumeric(valueType->name)))
    {
        return true;
    }

    const TypeName* valueInner = TypeNameBoxInner(valueType);

    return HandleExtendsFrom(&r->m_registry, valueType->name, targetType->name)
           || (valueInner && strcmp(valueInner->name, targetType->name) == 0);
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
        const TypeName* varType = (const TypeName*)StrMapGet(scope, ident->name);

        if (!varType)
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "unknown variable '%s'", ident->name);
        }
        else if (TypeNameIsOwning(varType))
        {
            /* Whole-value use: reject if fully or partially moved. Member-base use: reject only if fully moved (descend into partial). */
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

        if (!a->target || !a->value)
        {
            return;
        }

        CheckConstAssign(r, a->target, a->base.range);

        /* Reassigning a scalar local invalidates its index spellings' facts. */
        if (a->target->kind == NodeIdent)
        {
            InvalidateIndexVar(r, ((IdentExpr*)a->target)->name);
        }
        else if (a->target->kind == NodeIndex)
        {
            /* Writing an array element kills nested-index/length facts of the array root. */
            const char* targetKey = MovableBoxSourceKey(r, a->target);

            if (targetKey)
            {
                InvalidateIndexVar(r, KeyRoot(r->m_arena, targetKey));
            }
        }

        const TypeName* tt = (a->target->kind == NodeIdent) ? InferType(r, a->target, scope) : NULL;
        bool targetIsBox = tt && TypeNameIsOwning(tt);

            /* An owning field target is reassigned, not read - re-life it instead of tripping use-after-move. */
            const char* fieldKey = NULL;
        bool targetIsOwningField = false;

        if (a->target->kind == NodeMember)
        {
            const TypeName* ft = InferType(r, a->target, scope);

            if (ft && TypeNameIsOwning(ft))
            {
                targetIsOwningField = true;
                fieldKey = MovableBoxSourceKey(r, a->target);
            }

                /* Writing through an optional link requires every optional ancestor proven. */
                for (Node* anc = ((MemberExpr*)a->target)->base_node; anc;)
            {
                const TypeName* at2 = InferType(r, anc, scope);

            if (at2 && at2->isOptional && !IsPathNonEmpty(r, MovableBoxSourceKey(r, anc)))
            {
                DiagOptionalReadError(r, a->base.range, MovableBoxSourceKey(r, anc), at2->name);
            }

                if (anc->kind == NodeMember)
                {
                    anc = ((MemberExpr*)anc)->base_node;
                }
                else if (anc->kind == NodeIndex)
                {
                    anc = ((IndexExpr*)anc)->base_node;
                }
                else
                {
                    anc = NULL;
                }
            }
        }

        /* A bare braced RHS carries no type - infer it from the target. Non-array/struct targets reject it. */
        if ((a->value->kind == NodeArrayInit || a->value->kind == NodeStructInit)
            && (a->target->kind == NodeIdent || a->target->kind == NodeMember))
        {
            const TypeName* at = (a->target->kind == NodeIdent) ? tt : InferType(r, a->target, scope);
            const TypeName* atArr = at && at->isOptional ? at->inner : at;

                /* A braced RHS against a struct target is a struct init. */
                a->value = ApplyBracedStructTarget(r, a->value, atArr);

            if (a->value->kind == NodeArrayInit && at && TypeNameIsDynamicArray(atArr))
            {
                ArrayInitExpr* ai = (ArrayInitExpr*)a->value;

                if (!ai->elementType)
                {
                    ai->elementType = TypeNameArrayElem(atArr);
                }
            }
            else if (a->value->kind == NodeStructInit && !((StructInitExpr*)a->value)->typeName)
            {
                const char* targetKey = MovableBoxSourceKey(r, a->target);

                DiagErrorFmt(r->m_diag, a->value->range,
                             "cannot infer a struct type for the braced initializer assigned to '%s' ('%s')",
                             targetKey ? targetKey : "target", at ? at->name : "unknown");
            }
            else if (a->value->kind == NodeArrayInit && at && !TypeNameIsFixedArray(atArr))
            {
                const char* targetKey = MovableBoxSourceKey(r, a->target);

                DiagErrorFmt(r->m_diag, a->value->range,
                             "cannot assign a braced initializer to '%s' of non-array type '%s'",
                             targetKey ? targetKey : "target", at->name);
            }
        }

        if (targetIsBox)
        {
            /* Resolve the value first so a call's resolvedDecl is set before type inference. */
            ResolveExpr(r, a->value, scope);
            const TypeName* vt = InferType(r, a->value, scope);

            /* `=` rebinds the box; other assigns mutate contents. An optional target is always rebound (compound ops rejected). */
            bool optionalTarget = tt->isOptional;

            if (optionalTarget && a->op != AssignSet)
            {
                DiagErrorFmt(r->m_diag, a->base.range,
                             "cannot compound-assign into '%s' of nullable type '%s'; it has not been blessed",
                             ((IdentExpr*)a->target)->name, tt->name);

                return;
            }

            bool boxMove = a->op == AssignSet && vt && (strcmp(vt->name, tt->name) == 0 || optionalTarget);
            const char* targetName = ((IdentExpr*)a->target)->name;

            if (boxMove)
            {
                if (IsBoxGlobalName(r, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "box global '%s' cannot be reassigned; only its fields may be mutated", targetName);

                    return;
                }

                /* A `ref ^T` is the caller's box - rebinding would rebind the caller; only contents may be assigned. */
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
                /* Plain/compound assign into a ^T mutates contents in place (not a move). */
                if (IsBoxMoved(r, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "'%s' used after move", targetName);
                }

                const TypeName* inner = TypeNameBoxInner(tt);

                if (vt && inner && !IsAssignableType(r, inner, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' into '%s' '%s'", vt->name, inner->name,
                                 targetName);
                }

                /* Content-assigning a `T?` reads through it (inner for ^T, target for string). */
                CheckOptionalDeref(r, a->value, vt, inner ? inner : tt, a->base.range);
            }
        }
        else if (targetIsOwningField)
        {
            /* Writing an owning field re-lives it. Only the root needs a move-check; partial ancestors are fine. */
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

            /* Whole fixed-size array fields cannot be assigned (no splice
                semantics; element stores are the supported path). */
            if (a->target->kind == NodeMember || a->target->kind == NodeIndex)
            {
                const TypeName* mt = InferType(r, a->target, scope);

                if (TypeNameIsFixedArray(mt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "cannot assign to a whole fixed-size array of type '%s'; assign its elements instead",
                                 mt->name);
                }
            }

            /* `^T` into a plain `T` target reads through the box (same coercion as inits/calls/returns), not a move. */
            if (tt)
            {
                const TypeName* vt = InferType(r, a->value, scope);

                if (vt && !IsAssignableType(r, tt, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to '%s' of type '%s'", vt->name,
                                 ((IdentExpr*)a->target)->name, tt->name);
                }

                /* Assigning a `T?` into a plain `T` unwraps the optional. */
                CheckOptionalDeref(r, a->value, vt, tt, a->base.range);
            }
            else if (a->target->kind == NodeMember)
            {
                /* Non-owning field target (`bob.plain = opt;`) - the field
                    type is the unwrap target. */
                const TypeName* mt = InferType(r, a->target, scope);
                const TypeName* vt = InferType(r, a->value, scope);

                CheckOptionalDeref(r, a->value, vt, mt, a->base.range);
            }
        }

        return;
    }
    case NodeIncDec:
    {
        IncDecExpr* inc = (IncDecExpr*)n;
        CheckConstAssign(r, inc->operand, inc->base.range);
        ResolveExpr(r, inc->operand, scope);

        /* ++/-- invalidates index spellings of the operand. */
        if (inc->operand->kind == NodeIdent)
        {
            InvalidateIndexVar(r, ((IdentExpr*)inc->operand)->name);
        }
        else if (inc->operand->kind == NodeIndex)
        {
            /* ++/-- on an array element feeds nested-index spellings too. */
            const char* operandKey = MovableBoxSourceKey(r, inc->operand);

            if (operandKey)
            {
                InvalidateIndexVar(r, KeyRoot(r->m_arena, operandKey));
            }
        }

        return;
    }
    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;
        ResolveExpr(r, cast->operand, scope);

        const TypeName* src = InferType(r, cast->operand, scope);
        const TypeName* dst = &cast->type;

        const char* srcName = src ? src->name : "";
        const char* dstName = dst->name;

        bool scalarPair = src && IsScalarTypeName(srcName) && IsScalarTypeName(dstName);
        bool handlePair
            = src && IsHandleType(&r->m_registry, srcName) && IsHandleType(&r->m_registry, dstName)
              && (HandleExtendsFrom(&r->m_registry, dstName, srcName) || HandleExtendsFrom(&r->m_registry, srcName, dstName));

        /* ^T -> ^U only when T or U is opaque (erase/cast-back). */
        bool boxPair = src && TypeNameIsOwning(src) && TypeNameIsOwning(dst)
                       && ((TypeNameBoxInner(src) && TypeRegistryIsOpaque(&r->m_registry, src->inner->name))
                           || (TypeNameBoxInner(dst) && TypeRegistryIsOpaque(&r->m_registry, dst->inner->name)));

        if (src && !scalarPair && !handlePair && !boxPair)
        {
            DiagErrorFmt(r->m_diag, cast->base.range, "invalid cast from '%s' to '%s'", srcName, dstName);
        }

        return;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;
        ResolveExprImpl(r, m->base_node, scope, true);

        const TypeName* rawBaseType = InferType(r, m->base_node, scope);

        /* Reading a possibly-empty optional needs a narrowing fact. */
        if (rawBaseType && rawBaseType->isOptional)
        {
            const char* baseKey = MovableBoxSourceKey(r, m->base_node);

            if (!IsPathNonEmpty(r, baseKey))
            {
                DiagOptionalReadError(r, m->base.range, baseKey, rawBaseType->name);
            }
        }

        const TypeName* baseType = UnwrapBoxPtr(rawBaseType);

        if (baseType && TypeRegistryIsOpaque(&r->m_registry, baseType->name))
        {
            if (IsIncompleteStruct(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'", baseType->name);
            }
            else
            {
                DiagError(r->m_diag, m->base.range, "cannot access a member of an opaque handle");
            }
        }

        /* Box fields can be moved out too; IsBoxUnusable catches full or partial moves. */
        const TypeName* selfType = InferType(r, n, scope);

        if (selfType && TypeNameIsOwning(selfType))
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

        /* A context-free braced literal has no type yet - resolve field values; full checks run later. */
        if (!structInitExpr->typeName)
        {
            for (size_t i = 0; i < structInitExpr->fields.count; i++)
            {
                StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);

                if (field->value)
                {
                    ResolveExpr(r, field->value, scope);
                }
            }

            return;
        }

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

            if (fieldDecl && field->value)
            {
                /* A braced value against a struct-shaped field is a nested struct init. */
                if (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit)
                {
                    field->value = ApplyBracedStructTarget(r, field->value, &fieldDecl->type);
                }

                if (field->value->kind == NodeArrayInit)
                {
                    ArrayInitExpr* ai = (ArrayInitExpr*)field->value;

                    if (fieldDecl->type.isArray && fieldDecl->type.length >= 0)
                    {
                        /* Fixed array: shape must mirror type - nested rows per dimension, flat list for 1-D; rows place row-major. */
                        long total = 0;
                        const TypeName* leaf = FixedArrayLeaf(&fieldDecl->type, &total);

                        if (!ai->elementType)
                        {
                            ai->elementType = leaf;
                        }

                        if (!CheckFixedArrayInitShape(r, &fieldDecl->type, ai, leaf, fieldDecl->name))
                        {
                            ai->elements.count = 0;
                            continue;
                        }

                        /* Place rows at their row-major offsets; short rows
                            leave NULL holes and missing elements zero. */
                        {
                            Vec flat;
                            VecInit(&flat);

                            for (long z = 0; z < total; z++)
                            {
                                VecPush(&flat, NULL);
                            }

                            if (!PlaceFixedArrayInit(&flat, 0, &fieldDecl->type, ai))
                            {
                                DiagErrorFmt(r->m_diag, field->value->range,
                                             "too many initializers for fixed-size array field '%s' (%ld max)",
                                             fieldDecl->name, total);
                            }

                            void** oldItems = ai->elements.items;
                            ai->elements = flat;
                            free(oldItems);
                        }

                            /* Bare-brace leaves are struct inits of the leaf type; fill now. */
                            for (size_t k = 0; k < ai->elements.count; k++)
                        {
                            Node* elem = (Node*)VecGet(&ai->elements, k);

                            if (!elem)
                            {
                                continue;
                            }

                            if (elem->kind == NodeArrayInit || elem->kind == NodeStructInit)
                            {
                                VecSet(&ai->elements, k, ApplyBracedStructTarget(r, elem, leaf));
                            }
                        }

                        for (size_t k = 0; k < ai->elements.count; k++)
                        {
                            Node* elem = (Node*)VecGet(&ai->elements, k);

                            if (!elem)
                            {
                                continue;   /* hole: stays zero */
                            }

                            const TypeName* elemType = InferType(r, elem, scope);

                            if (elemType && !IsAssignableType(r, leaf, elemType))
                            {
                                DiagErrorFmt(r->m_diag, elem->range,
                                             "element of type '%s' cannot initialize '%s' element of field '%s'",
                                             elemType->name, leaf->name, fieldDecl->name);
                            }
                        }
                    }
                    else if (TypeNameIsDynamicArray(&fieldDecl->type))
                    {
                        /* Dynamic array field: braced literal allocates a fresh array; fill element type and check. */
                        const TypeName* elem = TypeNameArrayElem(&fieldDecl->type);

                        if (!ai->elementType)
                        {
                            ai->elementType = elem;
                        }

                        for (size_t k = 0; k < ai->elements.count; k++)
                        {
                            Node* element = (Node*)VecGet(&ai->elements, k);
                            const TypeName* elemType = InferType(r, element, scope);

                            if (elemType && !IsAssignableType(r, elem, elemType))
                            {
                                DiagErrorFmt(r->m_diag, element->range,
                                             "element of type '%s' cannot initialize '%s' element of field '%s'",
                                             elemType->name, elem->name, fieldDecl->name);
                            }
                        }
                    }
                    else
                    {
                        DiagErrorFmt(r->m_diag, field->value->range,
                                     "braced initializers are only supported for array fields; "
                                     "field '%s' has type '%s'",
                                     fieldDecl->name, fieldDecl->type.name);
                    }
                }
                else
                {
                    const TypeName* fieldValueType = InferType(r, field->value, scope);

                    if (fieldValueType && !IsAssignableType(r, &fieldDecl->type, fieldValueType))
                    {
                        DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                                     "field '%s' of struct '%s' cannot be initialized by expression of type '%s'",
                                     fieldDecl->name, structInitExpr->typeName, fieldValueType->name);
                    }

                    /* A plain `T` field initialized from a `T?` unwraps it. */
                    CheckOptionalDeref(r, field->value, fieldValueType, &fieldDecl->type, field->value->range);
                }

                /* A ^T field moves its source only when that source is itself owning. */
                const TypeName* fieldValueType2 = InferType(r, field->value, scope);

                const char* movedFieldKey
                    = (TypeNameIsOwning(&fieldDecl->type)
                       && fieldValueType2 && TypeNameIsOwning(fieldValueType2))
                      ? MovableBoxSourceKey(r, field->value) : NULL;

                if (movedFieldKey)
                {
                    MoveBoxIdent(r, movedFieldKey, structInitExpr->base.range);
                }
            }
        }

        /* Non-optional `^T` fields must be initialized (NULL would trip every use); `T?` may stay empty. */
        if (structType && !TypeRegistryIsOpaque(&r->m_registry, structInitExpr->typeName))
        {
            size_t positionalIndex = 0;

            for (size_t i = 0; i < structInitExpr->fields.count; i++)
            {
                StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);

                if (!field->name || field->name[0] == '\0')
                {
                    positionalIndex++;
                }
            }

            /* Re-walk to map literal entries onto struct fields the same way
               the checking loop above does. */
            size_t positionalSeen = 0;
            bool covered[256] = {false};
            bool overflow = structType->fields.count > 256;

            for (size_t i = 0; i < structInitExpr->fields.count && !overflow; i++)
            {
                StructInitField* field = (StructInitField*)VecGet(&structInitExpr->fields, i);
                size_t idx = (size_t)-1;

                if (!field->name || field->name[0] == '\0')
                {
                    idx = positionalSeen++;
                }
                else
                {
                    int named = TypeRegistryFieldIndex(&r->m_registry, structInitExpr->typeName, field->name);

                    if (named >= 0)
                    {
                        idx = (size_t)named;
                    }
                }

                if (idx < 256)
                {
                    covered[idx] = true;
                }
            }

            for (size_t f = 0; f < structType->fields.count && !overflow; f++)
            {
                FieldDecl* fd = (FieldDecl*)VecGet(&structType->fields, f);

                 /* Every OWNING field (`^T`, owning struct, `string`) must be
                    initialized; optionals and dynamic arrays may stay empty
                    (a T? is null, a zero-filled T[] is the canonical empty
                    {null, 0} fat struct). */
                bool mustInit = !fd->type.isOptional
                                && !TypeNameIsDynamicArray(&fd->type)
                                && (TypeNameIsOwning(&fd->type)
                                    || TypeRegistryIsOwningStruct(&r->m_registry, fd->type.name));

                if (mustInit && (f >= 256 || !covered[f]))
                {
                    /* Suggest the optional spelling of the inner type. A
                       `^string` field's optional is spelled `string?`. */
                    const TypeName* inner = TypeNameBoxInner(&fd->type);
                    const char* optSpelling = inner ? inner->name : fd->type.name;

                    DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                                 "owning field '%s' of struct '%s' must be initialized "
                                 "(declare it '%s?' if it may be empty)",
                                 fd->name, structInitExpr->typeName, optSpelling);
                }
            }

            if (overflow)
            {
                DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                             "struct '%s' has too many fields to check for missing initializers",
                             structInitExpr->typeName);
            }
        }

        return;
    }
    case NodeIndex:
    {
        IndexExpr* ix = (IndexExpr*)n;
        ResolveExprImpl(r, ix->base_node, scope, true);
        ResolveExprImpl(r, ix->index, scope, false);

        /* Indexing an optional array (`opt[i]`) reads its contents - the
            same narrowing rule as a member read through a `T?`. */
        {
            const TypeName* baseType = InferType(r, ix->base_node, scope);

            if (baseType && baseType->isOptional)
            {
                const char* baseKey = MovableBoxSourceKey(r, ix->base_node);

                if (!IsPathNonEmpty(r, baseKey))
                {
                    DiagOptionalReadError(r, ix->base.range, baseKey, baseType->name);
                }
            }
        }

        return;
    }
    case NodeNullTest:
    {
        NullTestExpr* nt = (NullTestExpr*)n;

        /* Every optional ANCESTOR of the tested path must already be proven non-empty, or the test itself faults. */
        Node* anc = NULL;

        if (nt->operand->kind == NodeMember)
        {
            anc = ((MemberExpr*)nt->operand)->base_node;
        }
        else if (nt->operand->kind == NodeIndex)
        {
            anc = ((IndexExpr*)nt->operand)->base_node;
        }

        for (; anc;)
        {
            const TypeName* at = InferType(r, anc, scope);

            if (at && at->isOptional && !IsPathNonEmpty(r, MovableBoxSourceKey(r, anc)))
            {
                DiagOptionalReadError(r, nt->base.range, MovableBoxSourceKey(r, anc), at->name);
            }

            if (anc->kind == NodeMember)
            {
                anc = ((MemberExpr*)anc)->base_node;
            }
            else if (anc->kind == NodeIndex)
            {
                anc = ((IndexExpr*)anc)->base_node;
            }
            else
            {
                anc = NULL;
            }
        }

        /* Finally resolve the operand path (gates apply within it). */
        ResolveExpr(r, nt->operand, scope);

        const TypeName* operandType = InferType(r, nt->operand, scope);

        if (operandType && !operandType->isOptional)
        {
            DiagErrorFmt(r->m_diag, nt->base.range,
                         "'?' test requires a nullable type ('T?'), but '%s' can never be empty",
                         operandType->name);
        }

        return;
    }
    case NodeArrayInit:
    {
        ArrayInitExpr* ai = (ArrayInitExpr*)n;

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, i);

            /* Nested array literal: propagate the element type's element into row literals. */
            if (ai->elementType && ai->elementType->isArray && elem->kind == NodeArrayInit
                && !((ArrayInitExpr*)elem)->elementType)
            {
                const TypeName* innerElem = TypeNameArrayElem(ai->elementType);

                if (innerElem)
                {
                    ((ArrayInitExpr*)elem)->elementType = innerElem;
                }
            }

            ResolveExpr(r, elem, scope);

            /* An owning value in an array literal moves its source (like a var-decl move). */
            if (ai->elementType && TypeNameIsOwning(ai->elementType))
            {
                const char* movedKey = MovableBoxSourceKey(r, elem);

                if (movedKey)
                {
                    MoveBoxIdent(r, movedKey, elem->range);
                }
            }

            /* A non-optional element initialized from a `T?` unwraps it. */
            CheckOptionalDeref(r, elem, InferType(r, elem, scope), ai->elementType, elem->range);
        }

        return;
    }
    default:
        return;
    }
}

/* A loop body repeats, so a move must stay valid across the back edge. Walk once muted (warmup) to carry state forward, then walk again for real; errors needing carried-over state surface on the second pass. */
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope, const char* condFactKey, bool condFactNegated)
{
    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    /* Muted warmup simulating a second iteration. */
    Vec liveLog;
    VecInit(&liveLog);
    r->m_liveLog = &liveLog;

    WalkStmt(r, body, scope);

    r->m_liveLog = NULL;
    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    /* Clear state of whole-reassigned bindings so the next iteration starts fresh (e.g. `cur = cur.next;`). */
    for (size_t i = 0; i < liveLog.count; i++)
    {
        const char* key = (const char*)VecGet(&liveLog, i);
        ClearBoxSubtree(r, key);
        ClearNullableFacts(r, key);
    }

    /* The condition's fact holds through the body (even if reassigned); negated proves EMPTY. */
    if (condFactKey)
    {
        if (condFactNegated)
        {
            MarkPathEmpty(r, condFactKey);
        }
        else
        {
            MarkPathNonEmpty(r, condFactKey);
        }
    }

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

        if (TypeTreeHasFixedArray(&vd->type))
        {
            DiagErrorFmt(r->m_diag, vd->base.range,
                         "local variable '%s' may not have a fixed-size array type ('%s'); "
                         "fixed-size arrays are only allowed as struct fields",
                         vd->name, vd->type.name);
        }

        /* Owning types need an init (arrays default empty; optionals may be empty). */
        if (TypeNameIsOwning(&vd->type) && !TypeNameIsDynamicArray(&vd->type) && !TypeNameIsOptional(&vd->type)
            && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "box variable '%s' must be initialized", vd->name);
        }

        if (vd->type.isConst && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "const variable '%s' must be initialized", vd->name);
        }

        if (TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "owning struct '%s' must be stored in a box; use '^%s'",
                         vd->type.name, vd->type.name);
        }

        /* An owning struct element would leak on drop - box it (^S[]). */
        {
            const TypeName* arrElem = TypeNameArrayElem(&vd->type);

            if (arrElem && TypeRegistryIsOwningStruct(&r->m_registry, arrElem->name))
            {
                DiagErrorFmt(r->m_diag, vd->base.range,
                              "owning struct '%s' must be stored in a box; use '^%s[]'",
                              arrElem->name, arrElem->name);
            }
        }

        if (vd->init)
        {
            ResolveExpr(r, vd->init, scope);
            const TypeName* initType = InferType(r, vd->init, scope);

            bool ok = initType && IsAssignableType(r, &vd->type, initType);

            if (initType && !ok)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "'%s' cannot be initialized by expression of type '%s'",
                             vd->type.name, initType->name);
            }

            /* Initializing a plain `T` from a `T?` unwraps the optional. */
            CheckOptionalDeref(r, vd->init, initType, &vd->type, vd->base.range);

            /* ^T init from a box source (identifier/field/cast) moves it. */
            const char* movedInitKey
                = initType && TypeNameIsOwning(&vd->type) && TypeNameIsOwning(initType) ? MovableBoxSourceKey(r, vd->init) : NULL;

            if (movedInitKey)
            {
                MoveBoxIdent(r, movedInitKey, vd->base.range);
            }
        }

        /* Fresh binding: clear stale moved-state, nullable facts, and index deps from any prior same-named var. */
        ClearBoxSubtree(r, vd->name);
        ClearNullableFacts(r, vd->name);
        InvalidateIndexVar(r, vd->name);

        /* An initialized declaration proves the binding non-empty. */
        if (vd->init)
        {
            MarkPathNonEmpty(r, vd->name);
        }

        StrMapPut(scope, vd->name, (void*)&vd->type);

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
            if (r->m_currentReturnType && strcmp(r->m_currentReturnType->name, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "void function cannot return a value");
            }

            ResolveExpr(r, rs->value, scope);

            const TypeName* typeName = InferType(r, rs->value, scope);

            if (typeName && strcmp(typeName->name, "void") == 0)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type 'void'");
            }

            /* Validate the returned expression against the function's declared return type */
            if (r->m_currentReturnType && strcmp(r->m_currentReturnType->name, "void") != 0 && typeName
                && !IsAssignableType(r, r->m_currentReturnType, typeName))
            {
                DiagErrorFmt(r->m_diag, rs->base.range,
                             "cannot return a value of type '%s' from a function returning '%s'", typeName->name,
                             r->m_currentReturnType->name);
            }

            /* Returning a `T?` from a `T` function unwraps the optional. */
            CheckOptionalDeref(r, rs->value, typeName, r->m_currentReturnType, rs->base.range);

            /* Only a move if the function itself returns ^T; otherwise
                it's a read (unless the inner type is owning). */
            const char* movedReturnKey = typeName && TypeNameIsOwning(typeName) ? MovableBoxSourceKey(r, rs->value) : NULL;

            if (movedReturnKey)
            {
                bool returnsSameBox = r->m_currentReturnType
                                      && strcmp(r->m_currentReturnType->name, typeName->name) == 0;

                if (returnsSameBox)
                {
                    MoveBoxIdent(r, movedReturnKey, rs->base.range);
                }
                else
                {
                    /* Owning values without a box inner (string, T[]) have
                        nothing to "read out" into a differently-typed return,
                        so boxInner is NULL and no inner matching applies. The
                        type-mismatch diagnostic was already emitted above. */
                    const TypeName* boxInner = TypeNameBoxInner(typeName);
                    const char* boxInnerName = boxInner ? boxInner->name : NULL;

                    bool innerIsOwning = boxInnerName && TypeRegistryIsOwningStruct(&r->m_registry, boxInnerName);
                    bool innerMatchesReturn = boxInnerName
                                              && (r->m_currentReturnType
                                                  && (strcmp(r->m_currentReturnType->name, boxInnerName) == 0
                                                      || (IsNumeric(boxInnerName)
                                                          && IsNumeric(r->m_currentReturnType->name))));

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

        /* `if (path?)` blesses the then-branch; `if (!path?)` blesses the else. Facts intersect at the join; moved-state unions. */
        bool factNegated = false;
        Node* factOperand = CondNullTestOperand(i->condition, &factNegated);
        const char* factKey = factOperand ? MovableBoxSourceKey(r, factOperand) : NULL;

        /* then/else are walked from the same pre-state; the result merges both. */
        StrMap beforeBranches;
        CopyStrMap(&r->m_movedBoxes, &beforeBranches);

        StrMap beforeFacts;
        CopyStrMap(&r->m_nonEmptyPaths, &beforeFacts);

        StrMap beforeEmpty;
        CopyStrMap(&r->m_emptyPaths, &beforeEmpty);

        if (factKey)
        {
            if (factNegated)
            {
                MarkPathEmpty(r, factKey);
            }
            else
            {
                MarkPathNonEmpty(r, factKey);
            }
        }

        WalkStmt(r, i->thenBranch, scope);

        StrMap afterThen;
        CopyStrMap(&r->m_movedBoxes, &afterThen);

        StrMap factsThen;
        CopyStrMap(&r->m_nonEmptyPaths, &factsThen);

        ReplaceStrMapContents(&r->m_movedBoxes, &beforeBranches);
        ReplaceStrMapContents(&r->m_nonEmptyPaths, &beforeFacts);
        /* Empty facts never escape their branch. */
        ReplaceStrMapContents(&r->m_emptyPaths, &beforeEmpty);

        /* The implicit/explicit else runs when the condition is false: establishes the opposite fact. */
        if (factKey && factNegated)
        {
            MarkPathNonEmpty(r, factKey);
        }

        if (i->elseBranch)
        {
            if (factKey && !factNegated)
            {
                MarkPathEmpty(r, factKey);
            }

            WalkStmt(r, i->elseBranch, scope);

            ReplaceStrMapContents(&r->m_emptyPaths, &beforeEmpty);
        }

        MergeMovedBoxes(&r->m_movedBoxes, &afterThen);

        /* Keep only paths proven non-empty on BOTH branches. */
        MergeNonEmptyFacts(&r->m_nonEmptyPaths, &factsThen);

        StrMapFree(&beforeBranches);
        StrMapFree(&afterThen);
        StrMapFree(&beforeFacts);
        StrMapFree(&factsThen);

        return;
    }
    case NodeWhile:
    {
        WhileStmt* w = (WhileStmt*)n;
        ResolveExpr(r, w->condition, scope);

        bool whileFactNegated = false;
        Node* whileFactOperand = CondNullTestOperand(w->condition, &whileFactNegated);
        const char* factKey = whileFactOperand ? MovableBoxSourceKey(r, whileFactOperand) : NULL;

        WalkLoopBody(r, w->body, scope, factKey, whileFactNegated);

        /* Nothing survives a loop: facts never hold after it. */
        ClearAllNonEmptyFacts(r);

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

        bool forFactNegated = false;
        Node* forFactOperand = CondNullTestOperand(fs->condition, &forFactNegated);
        const char* forFactKey = forFactOperand ? MovableBoxSourceKey(r, forFactOperand) : NULL;

        if (fs->update)
        {
            ResolveExpr(r, fs->update, scope);
        }

        WalkLoopBody(r, fs->body, scope, forFactKey, forFactNegated);

        /* Same rule as while: facts never survive a loop. */
        ClearAllNonEmptyFacts(r);

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
    StrMapInit(&r.m_nonEmptyPaths);
    StrMapInit(&r.m_indexDeps);
    StrMapInit(&r.m_boxGlobals);
    StrMapInit(&r.m_refBoxParams);
    StrMapInit(&r.m_typeCache);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (TypeNameIsOwning(&gd->type))
        {
            StrMapPut(&r.m_boxGlobals, gd->name, (void*)&gd->type);
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
            StrMapPut(&scope, gd->name, (void*)&gd->type);

            if (gd->type.isConst)
            {
                StrMapPut(&r.m_constVars, gd->name, (void*)1);
            }
        }

        ResetStrMap(&r.m_constVars);
        ResetStrMap(&r.m_movedBoxes);
        ResetStrMap(&r.m_refBoxParams);

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);
            StrMapPut(&scope, p->name, (void*)&p->type);

            if (p->type.isConst)
            {
                StrMapPut(&r.m_constVars, p->name, (void*)1);
            }

            if (TypeNameIsOwning(&p->type) && p->mod == ModRef)
            {
                StrMapPut(&r.m_refBoxParams, p->name, (void*)1);
            }
        }

        r.m_currentReturnType = &functionDecl->returnType;

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

        /* Layout conflicts are computed in the type registry; report them here where a source range is available. */
        const StructType* registered = TypeRegistryFind(&r.m_registry, sd->name);

        if (registered && registered->layoutError)
        {
            DiagErrorFmt(diag, sd->base.range, "%s", registered->layoutError);
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (IsIncompleteStruct(&r.m_registry, field->type.name))
            {
                DiagErrorFmt(diag, field->type.range, "field '%s' has incomplete type '%s'", field->name,
                             field->type.name);
            }

            if (field->type.isArray && field->type.length >= 0)
            {
                if (field->type.length < 1)
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' must have a length of at least 1", field->name);
                }

                long total = 0;
                const TypeName* leaf = FixedArrayLeaf(&field->type, &total);

                if (leaf->isArray)
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' may not contain a dynamic array", field->name);
                }
                else if (leaf->isBox || TypeNameIsOwning(leaf))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' may not own its elements ('%s' is owning); "
                                 "fixed-size arrays have no drop glue",
                                 field->name, leaf->name);
                }
                else if (TypeRegistryIsOwningStruct(&r.m_registry, leaf->name))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "fixed-size array field '%s' may not contain an owning struct ('%s'); "
                                 "fixed-size arrays have no drop glue",
                                 field->name, leaf->name);
                }
                else if (IsIncompleteStruct(&r.m_registry, leaf->name))
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

    {
        StrMap globalScope;
        StrMapInit(&globalScope);

        for (size_t i = 0; i < mod->globals.count; i++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);
            StrMapPut(&globalScope, gd->name, (void*)&gd->type);
        }

        /* Clear per-function move state left over from the last function walked. */
        ResetStrMap(&r.m_movedBoxes);

        for (size_t i = 0; i < mod->globals.count; i++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

            if (TypeTreeHasFixedArray(&gd->type))
            {
                DiagErrorFmt(diag, gd->base.range,
                             "global '%s' may not have a fixed-size array type ('%s'); "
                             "fixed-size arrays are only allowed as struct fields",
                             gd->name, gd->type.name);
            }

            /* Array globals default to an empty {null, 0} fat struct. */
            if (TypeNameIsDynamicArray(&gd->type))
            {
                if (gd->init)
                {
                    ResolveExpr(&r, gd->init, &globalScope);
                    const TypeName* initType = InferType(&r, gd->init, &globalScope);

                    if (initType && strcmp(initType->name, gd->type.name) != 0)
                    {
                        DiagErrorFmt(diag, gd->base.range, "global '%s' of type '%s' cannot be initialized by expression of type '%s'",
                                     gd->name, gd->type.name, initType->name);
                    }
                }

                continue;
            }

            if (!TypeNameIsOwning(&gd->type))
            {
                continue;
            }

            if (!gd->init)
            {
                DiagErrorFmt(diag, gd->base.range, "box global '%s' must be initialized", gd->name);
                continue;
            }

            const TypeName* boxInner = TypeNameBoxInner(&gd->type);

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

            const TypeName* initType = InferType(&r, gd->init, &globalScope);

            /* boxInner is NULL for `string` (no box inner), so only compare
                against it when present; otherwise the bare type must match. */
            bool ok = initType
                      && ((boxInner && strcmp(initType->name, boxInner->name) == 0)
                          || strcmp(initType->name, gd->type.name) == 0);

            if (initType && !ok)
            {
                DiagErrorFmt(diag, gd->base.range, "global '%s' cannot be initialized by expression of type '%s'",
                             gd->name, initType->name);
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

            if (TypeTreeHasFixedArray(&p->type))
            {
                DiagErrorFmt(diag, p->base.range,
                             "parameter '%s' may not have a fixed-size array type ('%s'); "
                             "fixed-size arrays are only allowed as struct fields",
                             p->name, p->type.name);
            }
        }

        if (TypeTreeHasFixedArray(&functionDecl->returnType))
        {
            DiagErrorFmt(diag, functionDecl->base.range,
                         "function '%s' may not return a fixed-size array type ('%s'); "
                         "fixed-size arrays are only allowed as struct fields",
                         functionDecl->name, functionDecl->returnType.name);
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
    StrMapFree(&r.m_nonEmptyPaths);
    StrMapFree(&r.m_indexDeps);
    StrMapFree(&r.m_boxGlobals);
    StrMapFree(&r.m_refBoxParams);
    StrMapFree(&r.m_typeCache);
    StrMapFree(&byMangled);
    TypeRegistryFree(&r.m_registry);
}
