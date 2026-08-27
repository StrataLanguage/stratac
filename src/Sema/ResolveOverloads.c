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
    StrMap m_indexDeps;     /* index variable name -> Vec<const char*> of fact keys
                                spelled with it ("i" -> {"arr[i].w"}); mutating the
                                variable (or passing it as a non-const ref) must
                                drop those facts */
    StrMap m_boxGlobals;
    StrMap m_refBoxParams;
    StrMap m_typeCache; /* canonical spelling -> interned TypeName tree */
    const TypeName* m_currentReturnType;
    Vec* m_liveLog; /* when set, MarkBoxLive records keys here (loop warmup) */
} Resolver;

/* Intern a TypeName tree for a canonical spelling. Expressions that need a
   synthesized type (literals, builtin results) share one tree per spelling. */
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

/* True if a fixed-size array dimension (`T[N]`) appears anywhere in a type
   tree (including inside boxes: `^T[N]` is an array of boxes). */
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

/* Descend through the leading fixed-size array dimensions of a field type,
   returning the innermost non-fixed node and the total element count
   (product of the dimensions, 1 when the type is not a fixed array). */
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

/* Places the leaves of a (shape-checked) fixed-array initializer into
   `flat` (pre-sized to the type's total element count) at their ROW-MAJOR
   offsets: a short row leaves NULL holes and the next row still starts at
   its own offset, matching C (`{ {1,2}, {4} }` for `int[2][3]` puts 4 at
   cells[1][0], not cells[0][2]). Returns false when an element would land
   beyond the type's capacity. */
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

/* Enforces the SHAPE of a fixed-size array initializer against its type:
   multidimensional fields require nested rows - one brace level per
   dimension (`{ {1,2,3}, {4,5,6} }` for `int[2][3]`; rows may be short,
   missing elements zero) - and single-dimension fields require a flat
   leaf list (a braced element is still fine when the leaf is a struct:
   it is a struct init). Returns false (with a diagnostic) on a violation. */
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

/* Is `child` a path at or below `parent`? Rules beyond a plain prefix:
   - a '.' or '[' after the prefix continues the path ("a" covers "a.b"
     and "a[3].b");
   - the erased any-element form "a[]" covers every "a[<anything>]" key
     ("a[]" covers "a[3].b", "a[i].w", and "a[]" itself).
   The '[' case matters because element facts/keys are spelled precisely
   ("a[3]", "a[i]") while whole-array actions clear from the bare root. */
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

/* Clears `key` and all its descendants from the move-state map.
   Used when a whole-value reassignment re-lives the binding. */
static void ClearBoxSubtree(Resolver* r, const char* key)
{
    StrMap* m = &r->m_movedBoxes;

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

/* Nullable (`T?`) narrowing facts - defined below, used by move tracking. */
static void MarkPathNonEmpty(Resolver* r, const char* key);
static void ClearNonEmptySubtree(Resolver* r, const char* key);
static void ClearEmptySubtree(Resolver* r, const char* key);

static void MarkBoxLive(Resolver* r, const char* name)
{
    ClearBoxSubtree(r, name);
    StrMapPut(&r->m_movedBoxes, name, (void*)2);

    if (r->m_liveLog)
    {
        VecPush(r->m_liveLog, (void*)name);
    }

    /* A whole-value reassignment installs a fresh, definitely-initialized
       value - the path is non-empty again (and so are no stale descendants). */
    ClearNonEmptySubtree(r, name);
    ClearEmptySubtree(r, name);
    MarkPathNonEmpty(r, name);
}

/* Reassigning a single owning field (e.g. `bob.name = ...`) re-lives that
   field. Each partially-moved (3) ancestor prefix whose moved descendants
   have all been restored is cleared too, so a whole-value use of the parent
   becomes valid again once every moved field has been reassigned. */
static void RevalidateOwningField(Resolver* r, const char* key)
{
    ClearBoxSubtree(r, key);
    /* Reassigning a single owning field re-installs a valid value there. */
    ClearNonEmptySubtree(r, key);
    ClearEmptySubtree(r, key);
    MarkPathNonEmpty(r, key);

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
   "arr[3]" / "arr[i]" / "arr[]" -> "arr", "g.arr[i].w" -> "g". Used to
   check the underlying binding (a global, or a ref ^T param), not just
   the literal access text, so the move ban applies transitively through
   struct fields and array elements. */
static const char* KeyRoot(Arena* arena, const char* key)
{
    const char* dot = strchr(key, '.');
    const char* base = dot ? arena_strndup(arena, key, (size_t)(dot - key)) : key;

    /* Strip the trailing bracket group, whatever it spells: the erased
       "[]", a literal "[3]", or a variable "[i]". */
    const char* open = strrchr(base, '[');

    if (open)
    {
        return arena_strndup(arena, base, (size_t)(open - base));
    }

    return base;
}

/* Marks every proper path prefix of `key` as partially moved (value 3),
    unless already fully moved (1). For "a[i].b.c": marks "a", "a[i]" and
    "a[i].b" - prefixes break at dots AND at index brackets. */
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

/* ---- Nullable (`T?`) narrowing facts ------------------------------------
   A path ("weap.model") maps to 1 while sema can prove the box is non-empty
   (inside `if (weap.model?) { ... }`, or after a definite assignment).
   Reading through an unproven optional is a compile error. This is the
   mirror image of move poisoning: facts must be invalidated by anything
   that could empty/rebind the path (move-out, reassignment, shadowing). */

static bool IsPathNonEmpty(const Resolver* r, const char* key);
static void MarkPathNonEmpty(Resolver* r, const char* key);
static void ClearNonEmptySubtree(Resolver* r, const char* key);

static bool IsPathNonEmpty(const Resolver* r, const char* key)
{
    return key && StrMapGet(&r->m_nonEmptyPaths, key) == (void*)1;
}

/* True when the key contains the erased any-element form ("a[]"). Such
   keys are never provable: the spelling came from an index expression we
   could not pin to a constant or a tracked variable. */
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

/* Registers the fact key's index-variable dependencies: every identifier
   token inside a bracket group names a local whose mutation (assignment,
   ++/--, non-const ref pass, or a length change for `.length` spellings)
   must later drop the fact. */
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

/* Drops every fact whose key was spelled with `[var]` - called when the
   variable is assigned, incremented, or passed as a non-const ref. */
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

        ClearNonEmptySubtree(r, key);
        ClearEmptySubtree(r, key);
    }

    StrMapPut(&r->m_indexDeps, var, NULL);
}

/* Drops `key` and all `key.*` descendants from the non-empty map. */
static void ClearNonEmptySubtree(Resolver* r, const char* key)
{
    if (!key)
    {
        return;
    }

    StrMap* m = &r->m_nonEmptyPaths;

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
   The else branch of `if (path?)` proves the path is empty. An empty fact
   never leaves its else block, and is dropped by anything that could give
   the path a value (assignment, move-in, shadowing). Its main consumer is
   diagnostics: reading through a definitely-empty path gets a sharper
   message than a merely-unproven one. */

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
    if (!key)
    {
        return;
    }

    StrMap* m = &r->m_emptyPaths;

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


/* Picks the right "empty" wording: a path narrowed by an enclosing else
   block IS empty; anything else merely MAY be. */
static const char* EmptyWording(const Resolver* r, const char* key)
{
    return IsPathDefinitelyEmpty(r, key) ? "is definitely empty" : "may be empty";
}

static const char* MovableBoxSourceKey(Resolver* r, Node* n);

/* Shared diagnostic for reading through an unproven optional. An erased
   key ("arr[]") means the index expression could not be pinned to a
   constant or tracked variable - no fact can ever be established for it,
   so point at the materialize-into-a-local idiom instead. */
static void DiagOptionalReadError(Resolver* r, SourceRange range, const char* key, const char* typeName)
{
    if (PathKeyIsErased(key))
    {
        size_t nlen = strlen(typeName);
        bool alreadyOptional = nlen > 0 && typeName[nlen - 1] == '?';

        DiagErrorFmt(r->m_diag, range,
                     "'%s' may be empty ('%s'); the array index is not a constant or a trackable variable - "
                     "move or copy the element into a local first ('%s x = %s...; if (x?) ...')",
                     key, typeName, alreadyOptional ? typeName : "T?", key);
        return;
    }

    DiagErrorFmt(r->m_diag, range,
                 "'%s' %s ('%s'); test it first: if (%s?) { ... }",
                 key ? key : "<expression>", EmptyWording(r, key), typeName,
                 key ? key : "<expr>");
}

/* Reading the CONTENTS of a maybe-empty optional - passing it where its
   non-optional inner `T` is expected (call arg, var init, assignment,
   return, array element), so the value gets unwrapped - requires a
   narrowing fact, exactly like a member read through a `T?`. Targets
   that keep the box shape (`T?`, `^T`) rebind/move and never read
   contents, so they are exempt. A NULL target means "unknown" and is
   skipped (callers that know a deref happens pass the inner type). */
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

/* Extracts the tested path from an if/while/for condition shaped like
   `path?` or `!path?` (any number of `!` wrappers - parity tracked).
   Returns the null-test OPERAND node, or NULL when the condition isn't
   a null test. `*negated` tells whether the condition asserts the path
   EMPTY (then-branch) rather than non-empty. */
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

/* Intersects two fact sets: a path stays definitely-non-empty only when BOTH
   branches prove it. Dual of MergeMovedBoxes (which unions badness). */
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

/* Marks 'name' moved, unless it's a box global or rooted in a `ref ^T`
   param - a borrow of the caller's box, which the callee doesn't own and
   can't validly move out of (the caller's own liveness tracking has no way
   to see a move that happens inside a different function). */
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
    /* Moving out also invalidates any "definitely non-empty" fact for the
        path and its descendants - a moved-from optional is empty again. */
    ClearNonEmptySubtree(r, name);
}

/* Moving out an OPTIONAL path (`T?`) differs from a ^T/string move: the
   source slot is nulled in place and left EMPTY - a legal state, not a
   broken one. The move is not tracked as "moved" at all: the source just
   becomes an unproven (in fact empty) optional, so later `path?` tests
   read false and un-narrowed reads error with the ordinary "may be
   empty" wording. Neither whole-value move restriction applies: the
   parent box is not poisoned, and moving through a `ref` or out of a box
   global is a visible in-place mutation, never a dangling owner. */
static void MoveOptionalSource(Resolver* r, const char* name, SourceRange range)
{
    (void)range;

    ClearNonEmptySubtree(r, name);
    ClearEmptySubtree(r, name);
}

/* True when `name` is a module-level global - its mutation inside this
   function is not observable/tracked, so it cannot serve as a precise
   (dependency-tracked) index spelling. */
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

/* Prints a stable, unambiguous spelling of an index expression for path
   keys: literals, locals, `.length` of a local array, unary -/+/~ and the
   integer binary operators over those. Binaries are FULLY parenthesized so
   distinct trees never collide ("(i+(1*2))" vs "((i+1)*2)").
   Returns NULL when the expression cannot be pinned (calls, comparisons,
   globals, other node kinds) - the caller then erases the key, which is
   conservative for moves and never provable as a fact. */
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

        /* A global mentions no observable mutation site: erase. */
        return IsModuleGlobalName(r, name) ? NULL : arena_format(r->m_arena, "%s", name);
    }

    case NodeMember:
    {
        /* `.length` of a BARE LOCAL only: member bases have no observable
           invalidation point for their length. */
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
        /* A nested index used AS an index (`foo[other[bar]]`): spell base
           and bracket recursively. The base must itself be spellable (a
           bare local array); element writes into it are invalidated by the
           assignment/++/-- hooks, and rebinds/builtins by the root hooks. */
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

/* Move-tracking key for 'n' (unwraps casts), or NULL if not movable.
    Identifier -> its name; member chain -> dotted key ("holder.gun"). */
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

        /* Spell the index precisely so distinct elements are distinct keys:
           a literal ("a[3]"), a local ("a[i]"), or any arithmetic/length
           expression over locals ("a[i + 1]", "a[b.length - 1]") - the
           latter two carry dependency-tracked variables. Anything the
           printer refuses (calls, comparisons, globals) erases to the
           any-element form ("a[]"), which is conservative for move
           poisoning and NEVER provable as a narrowing fact. The "[]"
           marker also keeps element-moves distinct from whole-array moves
           in the moved-state map, while KeyRoot still reduces every
           "base[...]" to "base" for the global / ref-param check. */
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

/* Returns the result type of `copy(arg)`, or NULL if this isn't one.
   copy returns the same type as its argument (e.g. copy(string) → string). */
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

/* Result type of an array builtin call, or NULL if `c` isn't one.
   array_push -> ulong (new length); array_pop -> element type; array_resize -> void. */
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

    const TypeName* arrType = InferType(r, arg0, scope);

    /* A `T[]?` receiver is an optional array: the builtin mutates its
       contents, so it derefs like any optional read (checked in
       CheckCallArgOptionalDerefs). Unwrap for the element type. */
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

    /* All three builtins can change the receiver's LENGTH: any fact whose
        key was spelled through `[recv.length ...]` is stale. (Element-value
        facts survive push; resize/pop drop those separately below.) */
    {
        const char* lenRecvKey = MovableBoxSourceKey(r, arg0);

        if (lenRecvKey)
        {
            InvalidateIndexVar(r, KeyRoot(r->m_arena, lenRecvKey));
        }
    }

    /* array_resize (shrink drops trailing elements) and array_pop (removes
        the last element) can empty indexed slots we hold facts about; the
        lengths aren't tracked, so drop every element fact of the receiver.
        array_push preserves existing element values - facts survive it. */
    if (isResize || isPop)
    {
        const char* recvKey = MovableBoxSourceKey(r, arg0);

        if (recvKey)
        {
            ClearNonEmptySubtree(r, recvKey);
            ClearEmptySubtree(r, recvKey);
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
            /* A narrowed `T?` pushes into a `^T[]` element slot: the
                narrowing fact proves the box exists, so the move may take
                it (leaving the source optional empty). */
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

        /* Pushing an owning value read out of an array element
            (a borrow) would duplicate the owning pointer - the source array
            keeps it too, so both would free it. Move it into a local first. */
        if (valueType && TypeNameIsOwning(valueType) && IsArrayElementBorrow(arg1))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push '%s' read from an array element - it would be owned by two arrays; "
                         "move it into a variable first",
                         valueType->name);
        }

        /* Deref check before the move tracking below clears the pushed
            optional's fact. */
        CheckCallArgOptionalDerefs(r, c, scope);

        /* Pushing an owning value (string/box) moves it into the array.
            An optional source moves out cleanly: it is left EMPTY (a legal
            state) and never poisons its parent box. */
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

/* Types the C ABI can carry through a bare extern `...` as a plain value:
   scalars, string (const char*), and handles. Structs, arrays, SIMD
   vectors, and void can't. */
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

/* Types that may be passed through a bare extern `...` (C varargs).
   Scalars, string, handles, and ^T (which derefs to its value) reduce
   to something the C ABI can carry; ^T is only allowed when T itself is
   scalar/string/handle, since a boxed struct would have to cross by value. */
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

/* Moves box/string sources into owned (non-ref) params, and into the
    collected array of a typed rest param when its element type is owning.
    Also performs call-side fact invalidation: a non-const `ref` argument
    may be mutated (or re-bound) by the callee, and any real call may
    mutate globals - narrowing facts that could observe those must go. */
static void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c)
{
    /* Any real (non-pseudo) call can rebind module globals between the
        caller's test and use: global-rooted narrowing facts never survive
        a call. Locals cannot be seen by the callee (no captures; only
        `ref` args below can be mutated). */
    for (size_t g = 0; g < r->m_mod->globals.count; g++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->m_mod->globals, g);

        ClearNonEmptySubtree(r, gd->name);
        ClearEmptySubtree(r, gd->name);
    }

    /* For a typed rest, the last param is the collected T[] array itself, so
        the arg at that slot is really the first ELEMENT. Only args before it
        map to named params; everything from the rest slot onward moves only if
        the ELEMENT type is owning (e.g. ^T...), not because T[] is. */
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = typedRest && best->params.count > 0 ? best->params.count - 1 : best->params.count;

    for (size_t j = 0; j < c->args.count; j++)
    {
        Node* arg = (Node*)VecGet(&c->args, j);

        /* A non-const `ref` slot hands the callee a mutable view: any fact
            under that path (including array-element facts spelled through
            it) may no longer hold, and an ident arg's use as an index
            variable is untrackable from here on. */
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
                    /* The callee may empty/rebind through the ref, and it may
                        write elements of a passed array - nested-index
                        spellings ("foo[other[bar]]") die with it. */
                    InvalidateIndexVar(r, KeyRoot(r->m_arena, refKey));
                    ClearNonEmptySubtree(r, refKey);
                    ClearEmptySubtree(r, refKey);
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

/* After resolution, every call argument whose type is `T?` but whose
   destination wants the CONTENTS (a plain `T` param, a typed rest's
   element, a struct-constructor field, an array_push element, or a bare
   extern `...` slot) must be proven non-empty - the call would unwrap
   the maybe-empty box. */
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
        size_t namedCount = typedRest && fd->params.count > 0 ? fd->params.count - 1 : fd->params.count;

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
                /* Bare extern `...`: the C ABI carries the unwrapped value,
                   so the optional's inner is the effective target. */
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

    /* array_push(arr, opt): the element slot is the target; a `T[]?`
       receiver is itself dereferenced (its contents are mutated). */
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

/* Rebuilds a braced list node as a positional StructInitExpr of `structName`.
   The parser cannot know a `{...}` in expression position initializes a
   struct rather than an array, so sema rewrites the node once the expected
   (target/param/field) type is known. Elements become positional fields. */
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

/* Applies a struct-shaped expected type to a context-free braced node:
   rewrites a braced array list into a positional struct init, or fills the
   missing typeName of a designator form parsed in expression position
   (`{ .a = 1 }`). Returns the (possibly new) node; unchanged when the
   expected type is not a defined struct. */
static Node* ApplyBracedStructTarget(Resolver* r, Node* node, const TypeName* target)
{
    const TypeName* unwrapped = target && target->isOptional ? target->inner : target;
    unwrapped = unwrapped && unwrapped->isBox ? unwrapped->inner : unwrapped;

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
        /* Constructor call FooBar(arg): an owning field initialized from an
            owning source is a move — enforce the same rules as var-decl and
            array_push (globals / ref params can't be moved). The optional
            deref check runs BEFORE the moves: moving out of a `T?` clears
            its narrowing fact, but the fact held at call time. */
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            /* A braced list against a STRUCT field is a positional struct
                init (`Outer({}, ...)`); against an ARRAY field it is an
                array literal. The parser can't know, so decide here - and
                before any type inference below. */
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

            if (resolvedArg != arg)
            {
                VecSet(&c->args, j, resolvedArg);
            }
            else if (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
            {
                /* A braced list argument initializes an array field
                    positionally (`FooBar("x", {1, 2})`): it carries no
                    element type from the parser, so infer it from the
                    field. The argument's own resolution pass already ran
                    with a NULL element type, so its per-element move
                    marking was skipped - do it here (mirrors the
                    NodeArrayInit resolution body). */
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

        /* Facts first, moves second: the loop below may move an argument
            out of a `T?` (clearing its narrowing fact), but the fact held
            at call time. */
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

    /* Already resolved on an earlier pass over this same call (e.g. the
       warmup walk in WalkLoopBody) - c->callee has been rewritten to the
       chosen overload's mangled name, so a fresh name lookup below would
       find nothing. Reuse the cached decl and just redo the move-tracking
       side effects, which do need to run again for the real pass. */
    if (c->resolvedDecl)
    {
        const FunctionDecl* best = c->resolvedDecl;

        /* Facts first, moves second: TrackCallArgMoves clears the narrowing
            fact of an optional it moves out of, but the fact held at call
            time. */
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
            const TypeName* argType = InferType(r, arg, scope);

            if (!argType)
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
                /* `Weapon` (boxed on the fly, like rest-tail elements) and
                    `^Weapon` (widened) both coerce into a `Weapon?` param
                    when the inner types match. */
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

                /* T coerces to ^T (implicit boxing), matching array
                    literals. Only valid for owned (non-ref) typed-rest tail
                    args, where the collector boxes each element inline and the
                    callee owns it; a ref rest borrows, so it can't box. */
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

    /* A braced call argument against a struct param (`take({});`) is a
       positional struct init, not an array literal - rewrite before the
       checks/moves below consult argument types. */
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

        /* Member access through an optional goes through the same box
           pointer as through a `^T`. */
        if (baseType && baseType->isOptional)
        {
            baseType = baseType->inner;
        }

        if (baseType && baseType->isBox)
        {
            baseType = baseType->inner;
        }

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

        /* A `T?` slot accepts a `^T` (and vice versa) when the inner types
           match - same runtime representation. The reverse direction is NOT
           accepted here on purpose: moving an optional into a non-optional
           box requires a narrowing fact, which the callers check. */
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

        if (!a->target || !a->value)
        {
            return;
        }

        CheckConstAssign(r, a->target, a->base.range);

        /* Assigning a scalar local re-binds it: every narrowing fact spelled
            with it as an index ("arr[i]") is stale. This covers `=`,
            compound assigns, and for-loop updates alike. */
        if (a->target->kind == NodeIdent)
        {
            InvalidateIndexVar(r, ((IdentExpr*)a->target)->name);
        }
        else if (a->target->kind == NodeIndex)
        {
            /* Writing an ARRAY ELEMENT (`other[j] = v`) changes the value of
                any nested-index spelling ("foo[other[bar]]") or length-based
                one, at an untrackable position: kill the array root's
                dependent facts. */
            const char* targetKey = MovableBoxSourceKey(r, a->target);

            if (targetKey)
            {
                InvalidateIndexVar(r, KeyRoot(r->m_arena, targetKey));
            }
        }

        const TypeName* tt = (a->target->kind == NodeIdent) ? InferType(r, a->target, scope) : NULL;
        bool targetIsBox = tt && TypeNameIsOwning(tt);

        /* An owning struct field target (`bob.name = ...` where `name` is a
            string/box/array) is reassigned, not read - so it must re-life the
            field rather than trip the "used after move" check that a plain
            read of the target would. */
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

            /* Writing THROUGH an optional link (addressing a field inside a
               maybe-empty box) needs every optional ancestor proven - same
               rule as a read, since codegen must compute the field address. */
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

        /* A bare braced RHS (`a = {1,2,3};`, `s.field = {...};`, `box = {};`)
            carries no type from the parser - infer it from the target so
            codegen and the type checks below see a real type. A non-array,
            non-struct target cannot take a braced list at all. */
        if ((a->value->kind == NodeArrayInit || a->value->kind == NodeStructInit)
            && (a->target->kind == NodeIdent || a->target->kind == NodeMember))
        {
            const TypeName* at = (a->target->kind == NodeIdent) ? tt : InferType(r, a->target, scope);
            const TypeName* atArr = at && at->isOptional ? at->inner : at;

            /* A braced RHS against a struct-shaped target (`foo = {};`,
                `boxVar = { ... };`, `opt = {};`) is a struct initializer,
                not an array literal. */
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
            /* Resolve the value first (so a call gets its resolvedDecl set)
                before inferring its type - otherwise an unresolved call's
                type reads as unknown, misclassifying the assignment below. */
            ResolveExpr(r, a->value, scope);
            const TypeName* vt = InferType(r, a->value, scope);

            /* `=` rebinds the box (moves in a new box of the same type);
                any other assignment into a ^T - including `=` with a
                plain T value, e.g. `x = 5;` - mutates its contents instead.
                An optional target (`T?`) may be empty, so its contents can
                never be mutated in place: every `=` rebinds the whole slot
                (drop old + take new), and compound ops are rejected. */
            bool optionalTarget = tt->isOptional;

            if (optionalTarget && a->op != AssignSet)
            {
                DiagErrorFmt(r->m_diag, a->base.range,
                             "cannot compound-assign into '%s' of nullable type '%s'; it may be empty",
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

                /* A `ref ^T` is a view of the caller's box binding, not
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
                    ^T (`x = 5;`, `val -= amt;`) mutates the boxed value
                    in place - not a move, so it's allowed even for a box
                    global or a moved-and-revalidated box. */
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

                /* Content-assigning a `T?` reads through the optional to
                    move its inner value out - the content target is the
                    box inner for ^T targets, or the target itself for a
                    plain owning `string`. */
                CheckOptionalDeref(r, a->value, vt, inner ? inner : tt, a->base.range);
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

            /* A ^T value assigned into a plain (non-box) T target - a
                ref T param, a local, a field - reads through the box (same
                "^T -> T" coercion already used for var-decl inits, call
                args, and returns), not a move. */
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

        /* ++/-- mutates the operand: any narrowing fact spelled with this
            variable as an index ("arr[i]") is stale. */
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

        /* Reading through a possibly-empty optional requires a narrowing
            fact (`if (path?)`) - or an explicit reassignment of it. */
        if (rawBaseType && rawBaseType->isOptional)
        {
            const char* baseKey = MovableBoxSourceKey(r, m->base_node);

            if (!IsPathNonEmpty(r, baseKey))
            {
                DiagOptionalReadError(r, m->base.range, baseKey, rawBaseType->name);
            }
        }

        const TypeName* baseType = rawBaseType;

        if (baseType && baseType->isOptional)
        {
            baseType = baseType->inner;
        }

        if (baseType && baseType->isBox)
        {
            baseType = baseType->inner;
        }

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

        /* A box-typed field can be moved out too - check the same way.
            IsBoxUnusable catches both fully-moved (exact field moved) and
            partially-moved (a descendant of this field was moved). */
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

        /* A context-free braced literal (`{ .a = 1 }` in expression
            position) has no type name yet - the surrounding
            call/assignment fills it. Resolve the field values only; the
            full checking runs on a later pass over the now-typed node. */
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
                /* A braced value against a struct-shaped field
                    (`Outer { .inner = {} };`) is a nested struct init, not
                    an array literal. */
                if (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit)
                {
                    field->value = ApplyBracedStructTarget(r, field->value, &fieldDecl->type);
                }

                if (field->value->kind == NodeArrayInit)
                {
                    ArrayInitExpr* ai = (ArrayInitExpr*)field->value;

                    if (fieldDecl->type.isArray && fieldDecl->type.length >= 0)
                    {
                        /* Fixed-size array field: the initializer's SHAPE
                            must mirror the type - nested rows (one brace
                            level per dimension, rows may be short) for
                            multidimensional fields, a flat leaf list for
                            single-dimension ones. Rows then place at their
                            row-major offsets; missing elements zero. */
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

                        /* Bare-brace leaf elements (`{ .v = 1 }`, `{}`) are
                            struct inits of the leaf type - the parser can't
                            know that deep inside row literals, so fill them
                            now that the leaf type is known. */
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
                        /* Dynamic array field: the braced literal allocates a
                            fresh array. Fill the element type for codegen and
                            check per-element types. */
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

                /* A ^T field moves its source — but only when the source
                    value itself is owning (^T into ^T, string into
                    ^string). A bare T boxed into a ^T field is a copy. */
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

        /* Non-optional box fields (`^T`) must be explicitly initialized:
           an omitted field would be zero-filled, leaving a NULL box that
           every subsequent operation would trip over. Optionals (`T?`)
           may legitimately stay empty. */
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

                /* Every OWNING field must be initialized: `^T`, owning
                    structs held by value, and `string`. Optionals (`T?`)
                    and dynamic arrays (`T[]`) may stay empty: a T? slot is
                    null, and a zero-filled T[] is the canonical empty
                    {null, 0} fat struct (same as an uninitialized local). */
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

        /* Testing `a.b.c?` reaches c by dereferencing a and a.b - so every
           optional ANCESTOR of the tested path must already be proven
           non-empty, otherwise the test itself would fault. */
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

            /* Nested array literal (`int[][] g = { {1, 2}, {3} }`): the
                parser types only the outermost literal, so propagate the
                element type's own element into the row literals. */
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

            /* An owning value (string/box) stored in an array literal moves
                its source, so mark it moved just like a var-decl move. */
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

/* A loop body runs repeatedly, so a box move it contains must stay valid
   across the "loop back" edge too - not just top-to-bottom once. Walk the
   body once with diagnostics suppressed to propagate the moved-state one
   iteration would leave behind (this is what the next iteration actually
   starts from), then walk it again for real; any move violation that only
   shows up because of that carried-over state - like moving the same box
   on every iteration of a loop - is caught by the second walk. */
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope, const char* condFactKey, bool condFactNegated)
{
    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    /* Warmup pass: simulate a second iteration with diagnostics muted, so
       genuinely unsound bodies still surface their errors below. */
    Vec liveLog;
    VecInit(&liveLog);
    r->m_liveLog = &liveLog;

    WalkStmt(r, body, scope);

    r->m_liveLog = NULL;
    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    /* Back-edge credit: anything the body REASSIGNED as a whole is fresh
       again at the top of the next iteration - clear its subtree (and stale
       narrowing facts). Without this, `cur = cur.next;` in a list walk reads
       as a use-after-move even though each iteration binds a fresh cell. */
    for (size_t i = 0; i < liveLog.count; i++)
    {
        const char* key = (const char*)VecGet(&liveLog, i);
        ClearBoxSubtree(r, key);
        ClearNonEmptySubtree(r, key);
        ClearEmptySubtree(r, key);
    }

    /* The condition is re-tested every iteration: its narrowing fact holds
        throughout the body even if the body reassigned that very path
        (`while (cur?) { ... cur = cur.next; }`). A negated condition
        (`while (!cur?)`) proves the path EMPTY inside the body instead. */
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

        /* Owning types must be initialized so they hold a valid heap pointer
            - except arrays, which default to an empty {null, 0} fat struct,
            and optionals (`T?`), for which empty is a valid state. */
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

        /* An owning struct held by value as an array element would leak its
            owning fields on drop - it must be boxed too (^S[]). */
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

        /* Fresh binding: clear any stale moved-state a same-named variable
            left behind (e.g. from a prior loop iteration or an earlier,
            unrelated declaration in this function) - including field-level
            partial-move markers and nullable narrowing facts. A fresh `i`
            also orphans facts spelled with the old `i` as an index. */
        ClearBoxSubtree(r, vd->name);
        ClearNonEmptySubtree(r, vd->name);
        ClearEmptySubtree(r, vd->name);
        InvalidateIndexVar(r, vd->name);

        /* An initialized declaration proves the binding non-empty - same as
           a whole-value assignment would (`Weapon? w = Weapon { ... };`
           establishes `w?` for everything that follows). */
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

        /* A direct `if (path?)` condition establishes a definitely-non-empty
            fact for the then-branch; the negated `if (!path?)` form mirrors
            it: then-branch proves EMPTY, else-branch proves non-empty. Facts
            from both branches are intersected at the join; moved-state keeps
            its own conservative union merge. */
        bool factNegated = false;
        Node* factOperand = CondNullTestOperand(i->condition, &factNegated);
        const char* factKey = factOperand ? MovableBoxSourceKey(r, factOperand) : NULL;

        /* then/else are mutually exclusive - each is walked from the same
            starting moved-state (not one after the other), and the state
            after the if is a conservative merge of both outcomes. */
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
        /* Empty facts never escape their branch - not even the then-branch
            of a negated test. */
        ReplaceStrMapContents(&r->m_emptyPaths, &beforeEmpty);

        /* The else side of the if - an EXPLICIT else block, or the implicit
            fall-through when there is none - runs only when the condition is
            false: `path?` false means the path is definitely empty, while
            `!path?` false means it is non-empty (this implicit-else fact is
            what makes `if (!p?) { p = v; }` prove `p` at the join). */
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

        /* Nothing survives a loop: the condition may have been false on the
           last check, and any fact established inside may not hold. */
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

        /* Layout conflicts (extern struct redeclarations, overlapping
           fieldoffsets, unsizeable field types) are computed in the type
           registry; report them here where a source range is available. */
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
        if (r.m_movedBoxes.cap > 0)
        {
            memset(r.m_movedBoxes.keys, 0, r.m_movedBoxes.cap * sizeof(const char*));
            r.m_movedBoxes.count = 0;
        }

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
