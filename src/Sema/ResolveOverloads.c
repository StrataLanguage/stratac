#include "Sema/ResolveOverloads.h"
#include "Codegen/TypeRegistry.h"

#include <assert.h>
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
    StrMap m_constGlobals; /* const scalar global name -> ConstGlobalVal* (manifest constants) */
    const TypeName* m_currentReturnType;
    Vec* m_liveLog; /* when set, MarkBoxLive records keys here (loop warmup) */
} Resolver;

/* A folded manifest constant: a `const` scalar global whose initializer is a
   compile-time constant. Emits no storage — the value is inlined at every
   use (the #define/enum alternative for C++ consumers). */
typedef struct
{
    long long i;
    double f;
    bool isFloat;
    bool isInt; /* integer-kind type: usable as a fixed-array dimension */
} ConstGlobalVal;

/* Integer constant arithmetic (two's complement). `div`/`mod`/`>>` apply
   C signed semantics; refuses division/modulo by zero and shifts whose
   count is negative or >= 64. */
static bool SemaFoldConstIntArith(BinaryOp op, unsigned long long a, unsigned long long b, long long* out)
{
    switch (op)
    {
    case BinAdd:
        *out = (long long)((unsigned long long)0 + a + b);
        return true;
    case BinSub:
        *out = (long long)((unsigned long long)0 + a - b);
        return true;
    case BinMul:
        *out = (long long)((unsigned long long)0 + a * b);
        return true;
    case BinDiv:
        if (b == 0)
        {
            return false;
        }

        *out = (long long)a / (long long)b;
        return true;
    case BinMod:
        if (b == 0)
        {
            return false;
        }

        *out = (long long)a % (long long)b;
        return true;
    case BinBitAnd:
        *out = (long long)(a & b);
        return true;
    case BinBitOr:
        *out = (long long)(a | b);
        return true;
    case BinBitXor:
        *out = (long long)(a ^ b);
        return true;
    case BinShl:
        if (b >= 64)
        {
            return false;
        }

        *out = (long long)((unsigned long long)0 + a << b);
        return true;
    case BinShr:
        if (b >= 64)
        {
            return false;
        }

        *out = (long long)a >> (long long)b;
        return true;
    default:
        return false;
    }
}

/* Float constant arithmetic (`+ - * /`); refuses division by zero so the
   fold never embeds an inf/nan literal. */
static bool SemaFoldConstFloatArith(BinaryOp op, double a, double b, double* out)
{
    switch (op)
    {
    case BinAdd:
        *out = a + b;
        return true;
    case BinSub:
        *out = a - b;
        return true;
    case BinMul:
        *out = a * b;
        return true;
    case BinDiv:
        if (b == 0.0)
        {
            return false;
        }

        *out = a / b;
        return true;
    default:
        return false;
    }
}

static bool SemaFoldConstCompare(BinaryOp op, long long a, long long b, bool* res)
{
    switch (op)
    {
    case BinEqEq:
        *res = a == b;
        return true;
    case BinNotEq:
        *res = a != b;
        return true;
    case BinLt:
        *res = a < b;
        return true;
    case BinLtEq:
        *res = a <= b;
        return true;
    case BinGt:
        *res = a > b;
        return true;
    case BinGtEq:
        *res = a >= b;
        return true;
    default:
        return false;
    }
}

static bool SemaFoldConstCompareFloat(BinaryOp op, double a, double b, bool* res)
{
    switch (op)
    {
    case BinEqEq:
        *res = a == b;
        return true;
    case BinNotEq:
        *res = a != b;
        return true;
    case BinLt:
        *res = a < b;
        return true;
    case BinLtEq:
        *res = a <= b;
        return true;
    case BinGt:
        *res = a > b;
        return true;
    case BinGtEq:
        *res = a >= b;
        return true;
    default:
        return false;
    }
}

/* Applies a binary operator to two folded values with C constant-expression
   semantics: usual arithmetic conversions (int promotes to double when the
   other side is float); bitwise/shift/modulo require integers; comparisons
   and logical ops yield 0/1. */
static bool SemaFoldConstBinary(BinaryOp op, ConstGlobalVal l, ConstGlobalVal r, ConstGlobalVal* out)
{
    bool lFloat = l.isFloat;
    bool rFloat = r.isFloat;

    switch (op)
    {
    case BinAdd:
    case BinSub:
    case BinMul:
    case BinDiv:
    {
        if (lFloat || rFloat)
        {
            double a = lFloat ? l.f : (double)l.i;
            double b = rFloat ? r.f : (double)r.i;

            out->i = 0;
            out->isFloat = true;
            return SemaFoldConstFloatArith(op, a, b, &out->f);
        }

        long long v;

        if (!SemaFoldConstIntArith(op, (unsigned long long)l.i, (unsigned long long)r.i, &v))
        {
            return false;
        }

        out->i = (long)v;
        out->f = 0.0;
        out->isFloat = false;
        return true;
    }
    case BinMod:
    case BinBitAnd:
    case BinBitOr:
    case BinBitXor:
    case BinShl:
    case BinShr:
    {
        if (lFloat || rFloat)
        {
            return false;
        }

        long long v;

        if (!SemaFoldConstIntArith(op, (unsigned long long)l.i, (unsigned long long)r.i, &v))
        {
            return false;
        }

        out->i = (long long)v;
        out->f = 0.0;
        out->isFloat = false;
        return true;
    }
    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
    {
        bool res;

        if (lFloat || rFloat)
        {
            double a = lFloat ? l.f : (double)l.i;
            double b = rFloat ? r.f : (double)r.i;

            if (!SemaFoldConstCompareFloat(op, a, b, &res))
            {
                return false;
            }
        }
        else if (!SemaFoldConstCompare(op, l.i, r.i, &res))
        {
            return false;
        }

        out->i = res ? 1 : 0;
        out->f = 0.0;
        out->isFloat = false;
        return true;
    }
    case BinLogicAnd:
    case BinLogicOr:
    {
        bool a = lFloat ? l.f != 0.0 : l.i != 0;
        bool b = rFloat ? r.f != 0.0 : r.i != 0;

        out->i = (op == BinLogicAnd) ? (a && b) : (a || b);
        out->f = 0.0;
        out->isFloat = false;
        return true;
    }
    default:
        return false;
    }
}

/* Folds a const-global initializer to a compile-time value. Handles int/
   float/bool literals, unary ops, scalar casts, the C constant-expression
   operators over folded operands, and references to earlier manifest
   constants (`const B = A | (1 << 2);`). */
static bool SemaFoldConstInit(Resolver* r, Node* n, ConstGlobalVal* out)
{
    if (!n)
    {
        return false;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        out->i = (long long)((IntLiteral*)n)->value;
        out->f = 0.0;
        out->isFloat = false;
        return true;
    case NodeFloatLiteral:
        out->f = ((FloatLiteral*)n)->value;
        out->isFloat = true;
        return true;
    case NodeBoolLiteral:
        out->i = ((BoolLiteral*)n)->value ? 1 : 0;
        out->f = 0.0;
        out->isFloat = false;
        return true;
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;

        if (!SemaFoldConstInit(r, u->operand, out))
        {
            return false;
        }

        switch (u->op)
        {
        case UnNeg:
            if (out->isFloat)
            {
                out->f = -out->f;
            }
            else
            {
                out->i = -out->i;
            }

            return true;
        case UnPos:
            return true;
        case UnNot:
            if (out->isFloat)
            {
                out->i = out->f == 0.0;
            }
            else
            {
                out->i = out->i == 0;
            }

            out->f = 0.0;
            out->isFloat = false;
            return true;
        case UnBitNot:
            if (out->isFloat)
            {
                return false;
            }

            out->i = ~out->i;
            return true;
        }

        return false;
    }
    case NodeBinary:
    {
        BinaryExpr* e = (BinaryExpr*)n;
        ConstGlobalVal l;
        ConstGlobalVal rhs;

        if (!SemaFoldConstInit(r, e->lhs, &l) || !SemaFoldConstInit(r, e->rhs, &rhs))
        {
            return false;
        }

        return SemaFoldConstBinary(e->op, l, rhs, out);
    }
    case NodeCast:
    {
        ConstGlobalVal inner = {0};

        if (!SemaFoldConstInit(r, ((CastExpr*)n)->operand, &inner))
        {
            return false;
        }

        /* `isInt` describes the DECLARED type (set by the caller before the
           fold); the value copy must not clobber it. */
        bool wasInt = out->isInt;
        *out = inner;
        out->isInt = wasInt;
        return true;
    }
    case NodeIdent:
    {
        ConstGlobalVal* prev = (ConstGlobalVal*)StrMapGet(&r->m_constGlobals, ((IdentExpr*)n)->name);

        if (!prev)
        {
            return false;
        }

        bool wasInt = out->isInt;
        *out = *prev;
        out->isInt = wasInt;
        return true;
    }
    default:
        return false;
    }
}

/* True when the (alias-resolved) type is an integer kind usable as a fixed
   array dimension. */
static bool IsConstDimType(const TypeRegistry* reg, const char* name)
{
    const char* leaf = TypeRegistryResolveAlias(reg, name);

    return strcmp(leaf, "int") == 0 || strcmp(leaf, "uint") == 0 || strcmp(leaf, "long") == 0
           || strcmp(leaf, "ulong") == 0 || strcmp(leaf, "byte") == 0 || strcmp(leaf, "sbyte") == 0
           || strcmp(leaf, "short") == 0 || strcmp(leaf, "ushort") == 0;
}

/* A `const` scalar global of any numeric type folds into the manifest table
   (floats/bools inline at uses too — they just cannot size arrays). */
static bool IsManifestConstType(const TypeRegistry* reg, const char* name)
{
    return IsNumeric(TypeRegistryResolveAlias(reg, name));
}

/* Resolves `[constName]` fixed-array dimensions on a type tree against the
   manifest-constant table. */
static bool SemaResolveConstDims(Resolver* r, TypeName* t)
{
    if (!t)
    {
        return true;
    }

    if (t->lengthName)
    {
        ConstGlobalVal* v = (ConstGlobalVal*)StrMapGet(&r->m_constGlobals, t->lengthName);

        if (!v || !v->isInt)
        {
            DiagErrorFmt(r->m_diag, t->range,
                         "array dimension '%s' is not a compile-time integer constant "
                         "(declare it 'const int %s = ...;')",
                         t->lengthName, t->lengthName);
            return false;
        }

        t->length = (long)v->i;
        char* dimName = t->lengthName;
        t->lengthName = NULL;

        if (t->length < 1)
        {
            DiagErrorFmt(r->m_diag, t->range,
                         "array dimension '%s' must be at least 1 (got %ld)",
                         dimName ? dimName : "?", t->length);
            return false;
        }

        /* Rebuild the canonical spelling with the resolved dimension. */
        char* open = strchr(t->name, '[');

        if (open)
        {
            char* close = strchr(open, ']');

            if (close)
            {
                t->name = arena_format(r->m_arena, "%.*s[%ld]%s", (int)(open - t->name), t->name, t->length,
                                       close + 1);
            }
        }
    }

    if (!SemaResolveConstDims(r, t->elem))
    {
        return false;
    }

    return true;
}

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

/* Places fixed-array init leaves into `flat` at row-major offsets; short rows leave NULL holes. Returns false if an
 * element lands beyond capacity. */
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

/* Verifies a fixed-array initializer's SHAPE: nested rows for multidimensional fields, a flat list for
 * single-dimension. Returns false (with a diagnostic) on mismatch. */
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

/* Owning-ness with alias resolution: `struct Name = string;` binds a type
   that moves, requires init, and drops exactly like its underlying type.
   Plain leaves are non-owning; a leaf alias is owning when its fully
   resolved underlying type is. */
static bool AliasIsOwning(const Resolver* r, const TypeName* t)
{
    if (!t || !t->name || TypeNameIsOwning(t))
    {
        return TypeNameIsOwning(t);
    }

    const char* leaf = TypeRegistryResolveAlias(&r->m_registry, t->name);

    if (!leaf || strcmp(leaf, t->name) == 0)
    {
        return false;
    }

    if (strcmp(leaf, "string") == 0)
    {
        return true;
    }

    TypeName parsed = TypeNameParse(r->m_arena, leaf);

    return TypeNameIsOwning(&parsed);
}

/* True when both spellings resolve to the same underlying type through the
   alias table. Identity is otherwise preserved (two distinct aliases of
   `string` never unify implicitly); this is the explicit-cast escape hatch
   (`(Name)s`, `(string)n`) and nothing more. */
static bool SameResolvedType(const Resolver* r, const char* a, const char* b)
{
    const char* la = a ? TypeRegistryResolveAlias(&r->m_registry, a) : NULL;
    const char* lb = b ? TypeRegistryResolveAlias(&r->m_registry, b) : NULL;

    return la && lb && strcmp(la, lb) == 0;
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

/* Is `child` at or below `parent`? A '.' or '[' after the prefix continues the path; the erased "a[]" form covers every
 * "a[...]" key. */
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

    /* Reassignment re-lives the binding and drops its stale nullable facts; whether it is non-empty again depends on
     * the value assigned (the caller re-blesses when the new value proves it). */
    ClearNullableFacts(r, name);
}

/* Reassigning one owning field re-lives it and any ancestor whose moved descendants are all restored. */
static void RevalidateOwningField(Resolver* r, const char* key)
{
    ClearBoxSubtree(r, key);

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

            if ((m->values[i] == (void*)1 || m->values[i] == (void*)3) && strncmp(k, prefix, plen) == 0
                && k[plen] == '.')
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

/* The binding a move key is rooted in ("holder.gun" -> "holder", "arr[3]" -> "arr") - used for global/ref-param checks.
 */
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

/* Marks every proper path prefix of `key` as partially moved (3), unless already fully moved (1). Prefixes break at
 * dots and brackets. */
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
   A path maps to 1 while provably non-empty. Reading an un-blessed optional is an error. Blessings die on any
   rebind/move/shadow, like move poisoning. */

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
   The else branch of `if (path?)` proves the path empty. Never leaves its block; dropped by any rebind. Sharper
   diagnostics than a merely-unproven path. */

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

/* Diagnostic for reading an un-blessed optional. An erased key ("arr[]") can never be blessed - suggest materializing
 * into a local. */
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

    DiagErrorFmt(r->m_diag, range, "'%s' %s ('%s'); test it first: if (%s?) { ... }", key ? key : "<expression>",
                 EmptyWording(r, key), typeName, key ? key : "<expr>");
}

/* Reading optional CONTENTS into a non-optional target needs a narrowing fact. Box-shaped targets (`T?`/`^T`) rebind,
 * so exempt. */
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
                     "'%s' cannot be moved as it is not owned because it is global. (use a `[const] ref ^T`, copy() "
                     "it, or pass `T` by value)",
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

    /* The source is now definitely empty: later `path?` tests resolve to the
       "definitely empty" fact rather than an unproven read. */
    MarkPathEmpty(r, name);
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

/* Stable index spelling for path keys: literals, locals, `.length`, unary, and integer binaries (fully parenthesized).
 * Returns NULL when unpinnable - caller erases the key (conservative for moves). */
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

    /* Impl property reads are call-like (the getter returns a fresh value),
       never moves out of the receiver. */
    if (moved->kind == NodeMember && ((const MemberExpr*)moved)->isImplProperty)
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

        /* Spell the index precisely (literal/local/arithmetic) so distinct elements are distinct keys; unpinnable
         * spellings erase to "a[]" (conservative, never provable). "[]" also separates element moves from whole-array
         * moves. */
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

typedef struct AllowList
{
    const char** allowed;
    int size;
} AllowList;

/* Checks arg types for a function call, allowing any order. Returns the incorrect argument type if fails, NULL
 * otherwise. */
static bool CheckAllowListSingle(Resolver* r, CallExpr* c, StrMap* scope, const char** allowed, const int allowedSize)
{
    const int maxAllowed = 32;

    bool typesDepleted[32];
    memset(typesDepleted, 0, sizeof(typesDepleted));

    assert(allowedSize < maxAllowed);

    for (size_t i = 0; i < c->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, i);

        const TypeName* argType = InferType(r, arg, scope);

        if (!argType)
        {
            continue;
        }

        bool isCorrectType = false;

        /* Check in the allowed types */
        for (int j = 0; j < allowedSize; j++)
        {
            if (typesDepleted[j] == true)
            {
                /* Skip any depleted type */
                continue;
            }

            if (strcmp(argType->name, allowed[j]) == 0)
            {

                isCorrectType = true;
                typesDepleted[j] = true;

                break;
            }
        }

        if (isCorrectType == false)
        {
            /* Return the incorrect argument type */
            return false;
        }
    }

    return true;
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

static bool CheckArgTypesUnordered(Resolver* r, CallExpr* c, StrMap* scope, const AllowList* allowed,
                                   const int allowedSize)
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
    DiagErrorFmt(r->m_diag, c->base.range, "no matching call '%s'", callSig.data);
    free((void*)callSig.data);

    DiagNoteFmt(r->m_diag, c->base.range, "did you mean %s?", formatBuffer);

    return false;
}

static bool SimdVector2ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float", "float"}, 2},
    };

    return CheckArgTypesUnordered(r, c, scope, valid, sizeof(valid) / sizeof(valid[0]));
}

static bool SimdVector3ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float3"},                  1}, /* float3(float3(1.0, 2.0, 3.0)) */
        {(const char*[]){"float2", "float"},         2}, /* float3(float2(1.0, 2.0), 3.0) */
        {(const char*[]){"float", "float", "float"}, 3}, /* float3(1.0, 2.0, 3.0) */
    };

    return CheckArgTypesUnordered(r, c, scope, valid, sizeof(valid) / sizeof(valid[0]));
}

static bool SimdVector4ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float4"},                           1}, /* float4(float4(1.0, 2.0, 3.0, 4.0)) */
        {(const char*[]){"float3", "float"},                  2}, /* float4(float3(1.0, 2.0, 3.0), 4.0) */
        {(const char*[]){"float2", "float", "float"},         3}, /* float4(float2(1.0, 2.0), 3.0, 4.0) */
        {(const char*[]){"float2", "float2"},                 2}, /* float4(float2(1.0, 2.0), float2(3.0, 4.0)) */
        {(const char*[]){"float", "float", "float", "float"}, 4}, /* float4(1.0, 2.0, 3.0, 4.0) */
    };

    return CheckArgTypesUnordered(r, c, scope, valid, sizeof(valid) / sizeof(valid[0]));
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

    bool isValidCall = false;

    if (numLanes == 2)
    {
        isValidCall = SimdVector2ConstructValidate(r, c, scope);
    }
    else if (numLanes == 3)
    {
        isValidCall = SimdVector3ConstructValidate(r, c, scope);
    }
    else if (numLanes == 4)
    {
        isValidCall = SimdVector4ConstructValidate(r, c, scope);
    }

    if (!isValidCall)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "no matching call to '%s' with %zu arguments", c->callee, c->args.count);
        return false;
    }

    c->isPseudoCall = true;

    return true;
}

//-- SIMD vector dot / cross intrinsics.

/* Returns true if the module declares any user-defined function with `name`.
   `dot`/`cross` are only intrinsic fallbacks when no user overload exists, so a
   user's `dot(Vec3, Vec3)` is never hijacked by the SIMD resolver. */
static bool ModuleHasFunctionNamed(Resolver* r, const char* name)
{
    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* fd = (FunctionDecl*)VecGet(&r->m_mod->functions, i);
        if (strcmp(fd->name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Validates a `dot(a, b)` / `cross(a, b)` intrinsic call and marks it pseudo.
   Returns true if `c` was one (valid, or a misused dot/cross that was reported). */
static bool ResolveVectorIntrinsics(Resolver* r, CallExpr* c, StrMap* scope)
{
    bool isDot = strcmp(c->callee, "dot") == 0;
    bool isCross = strcmp(c->callee, "cross") == 0;

    if (!isDot && !isCross)
    {
        return false;
    }

    /* A user-defined overload takes precedence over the SIMD intrinsic. */
    if (ModuleHasFunctionNamed(r, c->callee))
    {
        return false;
    }

    if (c->args.count != 2)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects 2 arguments, got %zu", c->callee, c->args.count);
        return true;
    }

    Node* a0 = (Node*)VecGet(&c->args, 0);
    Node* a1 = (Node*)VecGet(&c->args, 1);

    const TypeName* t0 = InferType(r, a0, scope);
    const TypeName* t1 = InferType(r, a1, scope);

    /* Resolve type aliases to check for SIMD vector compatibility. */
    const char* resolved0 = t0 ? TypeRegistryResolveAlias(&r->m_registry, t0->name) : "";
    const char* resolved1 = t1 ? TypeRegistryResolveAlias(&r->m_registry, t1->name) : "";
    int lanes0 = GetSimdVectorLanes(resolved0);
    int lanes1 = GetSimdVectorLanes(resolved1);

    if (lanes0 == 0 || lanes1 == 0)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects SIMD vector arguments (float2/float3/float4), not '%s'",
                     c->callee, t0 ? t0->name : "?");
        return true;
    }

    if (lanes0 != lanes1)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' requires both vectors to have the same lane count ('%s' vs '%s')",
                     c->callee, t0->name, t1->name);
        return true;
    }

    c->isPseudoCall = true;

    return true;
}

//-- Array intrinsics.

/* Result type of `copy(arg)`, or NULL if not a copy call. */
static const TypeName* CopyBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0)
    {
        return NULL;
    }
    if (c->args.count != 1)
    {
        return NULL;
    }
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    return InferType(r, arg0, scope);
}

static bool ResolveCopyBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "copy") != 0)
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'copy' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (!argType || !AliasIsOwning(r, argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'copy' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    c->isPseudoCall = true;
    return true;
}

/* Result type of `drop(arg)` — always void (NULL TypeName). */
static const TypeName* DropBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    (void)r; (void)c; (void)scope;
    return NULL;
}

static bool ResolveDropBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "drop") != 0)
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'drop' expects 1 argument, got %zu", c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (!argType || !AliasIsOwning(r, argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'drop' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    c->isPseudoCall = true;

    /* Move the source — the box is invalidated after drop(). An optional
       source is left definitely EMPTY (legal to test later), never poisoned,
       like any other move out of a T?. */
    const char* movedKey = MovableBoxSourceKey(r, arg0);
    if (movedKey)
    {
        if (argType->isOptional)
        {
            MoveOptionalSource(r, movedKey, arg0->range);
        }
        else
        {
            MoveBoxIdent(r, movedKey, arg0->range);
        }
    }

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
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects %zu argument(s) but got %zu", c->callee, wantArgs,
                     c->args.count);
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

    /* Length may change: drop facts spelled through `[recv.length ...]`. Push keeps element facts; resize/pop drop them
     * below. */
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
            DiagErrorFmt(r->m_diag, arg1->range, "'array_resize' size must be an integer, not '%s'", sizeType->name);
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
                DiagErrorFmt(r->m_diag, arg1->range, "cannot push a value of type '%s' into '%s' (element type '%s')",
                             valueType->name, arrType->name, elemType->name);
            }
        }

        /* Pushing a borrow out of an array element would duplicate ownership; move it to a local first. */
        if (valueType && AliasIsOwning(r, valueType) && IsArrayElementBorrow(arg1))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push '%s' read from an array element - it would be owned by two arrays; "
                         "move it into a variable first",
                         valueType->name);
        }

        /* Deref check before the move tracking below clears the pushed optional's fact. */
        CheckCallArgOptionalDerefs(r, c, scope);

        /* Pushing an owning value moves it in; an optional source is left EMPTY, never poisoning its parent. */
        if (valueType && AliasIsOwning(r, valueType))
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

    if (IsNumeric(type->name) || strcmp(TypeRegistryResolveAlias(&r->m_registry, type->name), "string") == 0)
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

/* Moves box/string sources into owned params and rest-collected arrays. Also invalidates narrowing facts a real call
 * could break (non-const ref mutation, global rebinds). */
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
            moves = AliasIsOwning(r, &param->type) && param->mod == ModNone && !best->isExtern;
        }
        else if (typedRest)
        {
            const ParamDecl* restParam = (ParamDecl*)VecGet(&best->params, best->params.count - 1);
            const TypeName* elem = TypeNameArrayElem(&restParam->type);

            targetType = elem;
            moves = targetType && AliasIsOwning(r, targetType) && restParam->mod == ModNone && !best->isExtern;
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

/* Every `T?` arg whose target wants the CONTENTS (T param, rest element, struct field, array_push element, or extern
 * `...`) must be proven non-empty. */
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
        const TypeName* elem
            = arrType && arrType->isOptional ? TypeNameArrayElem(arrType->inner) : TypeNameArrayElem(arrType);
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

/* Applies a struct target type to a context-free braced node, or fills a designator's typeName. Unchanged if target
 * isn't a defined struct. */
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

// -- Impl blocks (methods + properties on handle types)

/* Finds the first impl block for `handleName` (NULL if none). */
static ImplDecl* FindImplDecl(Resolver* r, const char* handleName)
{
    for (size_t i = 0; i < r->m_mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&r->m_mod->impls, i);

        if (strcmp(impl->handleName, handleName) == 0)
        {
            return impl;
        }
    }

    return NULL;
}

/* Method lookup with `extends` walk: a Player sees impl Entity methods. */
static FunctionDecl* FindImplMethod(Resolver* r, const char* handleName, const char* methodName)
{
    const char* hn = handleName;
    size_t depth = 0;

    while (hn && depth <= r->m_registry.count)
    {
        for (size_t i = 0; i < r->m_mod->impls.count; i++)
        {
            ImplDecl* impl = (ImplDecl*)VecGet(&r->m_mod->impls, i);

            if (strcmp(impl->handleName, hn) != 0)
            {
                continue;
            }

            for (size_t j = 0; j < impl->methods.count; j++)
            {
                FunctionDecl* fn = (FunctionDecl*)VecGet(&impl->methods, j);

                if (fn->methodName && strcmp(fn->methodName, methodName) == 0)
                {
                    return fn;
                }
            }
        }

        const StructType* t = TypeRegistryFind(&r->m_registry, hn);
        hn = (t && t->opaque && t->extendsFrom) ? t->extendsFrom : NULL;
        depth++;
    }

    return NULL;
}

/* Property lookup on the exact handle (no inheritance). */
static PropertyDecl* FindImplProperty(Resolver* r, const char* handleName, const char* name)
{
    ImplDecl* impl = FindImplDecl(r, handleName);

    if (!impl)
    {
        return NULL;
    }

    for (size_t i = 0; i < impl->properties.count; i++)
    {
        PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, i);

        if (strcmp(prop->name, name) == 0)
        {
            return prop;
        }
    }

    return NULL;
}

/* If `n` is a property read on a handle (after unwrapping `T?`/`^T`), returns
   the property; NULL when the member is not an impl property. */
static PropertyDecl* PropertyOnHandle(Resolver* r, const TypeName* baseType, const char* member)
{
    const TypeName* inner = UnwrapBoxPtr(baseType);

    if (!inner || !IsHandleType(&r->m_registry, inner->name))
    {
        return NULL;
    }

    return FindImplProperty(r, inner->name, member);
}

static void VecPushFront(Vec* v, void* item)
{
    VecPush(v, NULL);
    memmove(v->items + 1, v->items, (v->count - 1) * sizeof(void*));
    v->items[0] = item;
}

/* Resolves `expr.Member(args...)` / `Type.Member(args...)` against impl blocks.
   Returns true when the call was fully handled (diagnostic emitted); false
   after a successful rewrite (fall through to normal overload resolution). */
static bool ResolveMemberCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    Node* base = c->calleeBase;

    /* `Type.Method(args)` — static call when the base is a type name. */
    bool isStatic = base->kind == NodeIdent && TypeRegistryIsUserType(&r->m_registry, ((IdentExpr*)base)->name);

    const char* handleName = NULL;

    if (isStatic)
    {
        handleName = ((IdentExpr*)base)->name;
    }
    else
    {
        ResolveExpr(r, base, scope);

        const TypeName* raw = InferType(r, base, scope);

        if (raw && raw->isOptional)
        {
            /* Reading through a `T?` receiver needs a narrowing fact. */
            const char* key = MovableBoxSourceKey(r, base);

            if (!IsPathNonEmpty(r, key))
            {
                DiagOptionalReadError(r, c->base.range, key, raw->name);
            }
        }

        const TypeName* inner = UnwrapBoxPtr(raw);

        if (!inner || !IsHandleType(&r->m_registry, inner->name))
        {
            DiagErrorFmt(r->m_diag, c->base.range, "type '%s' has no method '%s'", raw && inner ? inner->name : "?",
                         c->callee);
            return true;
        }

        handleName = inner->name;
    }

    FunctionDecl* method = FindImplMethod(r, handleName, c->callee);

    if (!method)
    {
        if (FindImplProperty(r, handleName, c->callee))
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'%s' is a property of '%s', not a method; access it without ()",
                         c->callee, handleName);
        }
        else
        {
            DiagErrorFmt(r->m_diag, c->base.range, "handle '%s' has no method '%s'", handleName, c->callee);
        }

        return true;
    }

    /* Instance call: when the method's first parameter is the receiver slot
       (a handle the receiver's type extends or equals), prepend the base
       expression as the self argument. Parameterless methods (factories) and
       static calls map arguments 1:1. */
    if (!isStatic && method->params.count > 0)
    {
        const ParamDecl* p0 = (const ParamDecl*)VecGet(&method->params, 0);

        if (IsHandleType(&r->m_registry, p0->type.name)
            && (strcmp(p0->type.name, handleName) == 0 || HandleExtendsFrom(&r->m_registry, handleName, p0->type.name)))
        {
            VecPushFront(&c->args, c->calleeBase);
        }
    }

    c->calleeBase = NULL;
    c->isPseudoCall = false;
    c->callee = method->name; /* qualified extern symbol, e.g. Camera_GetFOV */

    return false;
}

/* Synthesizes the extern declaration a property accessor refers to. When a
   function with that name already exists (e.g. a flat extern), its signature
   is validated against the accessor shape instead. */
static void SynthesizeAccessor(Module* mod, Arena* arena, const char* symbol, const TypeName* propType,
                               const char* handleName, bool isSetter, SourceRange range, DiagnosticEngine* diag)
{
    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(fn->name, symbol) != 0)
        {
            continue;
        }

        /* Validate the pre-existing declaration against the accessor shape:
           getter (self) -> propType; setter (self, value propType) -> void. */
        size_t wantParams = isSetter ? 2 : 1;

        if (fn->params.count != wantParams)
        {
            DiagErrorFmt(diag, range,
                         "property %s '%s' must take (%s self%s); found %zu parameter(s)",
                         isSetter ? "setter" : "getter", symbol, handleName, isSetter ? ", value" : "",
                         fn->params.count);
            return;
        }

        const ParamDecl* p0 = (const ParamDecl*)VecGet(&fn->params, 0);

        if (strcmp(p0->type.name, handleName) != 0)
        {
            DiagErrorFmt(diag, range, "property %s '%s' must take '%s' as its first parameter; found '%s'",
                         isSetter ? "setter" : "getter", symbol, handleName, p0->type.name);
            return;
        }

        if (isSetter && strcmp(fn->returnType.name, "void") != 0)
        {
            DiagErrorFmt(diag, range, "property setter '%s' must return void; found '%s'", symbol,
                         fn->returnType.name);
            return;
        }

        if (!isSetter && strcmp(fn->returnType.name, propType->name) != 0)
        {
            DiagErrorFmt(diag, range, "property getter '%s' must return '%s'; found '%s'", symbol, propType->name,
                         fn->returnType.name);
        }

        return;
    }

    FunctionDecl* fn = AST_NEW(arena, FunctionDecl);
    fn->base.kind = NodeFunction;
    fn->base.range = range;
    fn->name = arena_strdup(arena, symbol);
    fn->mangledName = fn->name;
    fn->isExtern = true;
    fn->fromImpl = true;
    VecInit(&fn->params);

    if (isSetter)
    {
        fn->returnType.name = arena_strdup(arena, "void");
    }
    else
    {
        fn->returnType = *propType;
    }

    ParamDecl* self = AST_NEW(arena, ParamDecl);
    self->base.kind = NodeParam;
    self->base.range = range;
    self->mod = ModNone;
    self->type = TypeNameLeaf(arena_strdup(arena, handleName));
    self->name = arena_strdup(arena, "self");
    VecPush(&fn->params, self);

    if (isSetter)
    {
        ParamDecl* value = AST_NEW(arena, ParamDecl);
        value->base.kind = NodeParam;
        value->base.range = range;
        value->mod = ModNone;
        value->type = *propType;
        value->name = arena_strdup(arena, "value");
        VecPush(&fn->params, value);
    }

    VecPush(&mod->functions, fn);
}

/* Validates impl targets and materializes property accessor externs (before
   the overload/mangling pass, so accessors flow through the normal pipeline). */
static void ResolveImpls(Module* mod, DiagnosticEngine* diag, Arena* arena, const TypeRegistry* registry)
{
    for (size_t i = 0; i < mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, i);

        if (!IsHandleType(registry, impl->handleName))
        {
            DiagErrorFmt(diag, impl->base.range, "impl type '%s' is not a declared handle", impl->handleName);
            continue;
        }

        for (size_t j = 0; j < impl->properties.count; j++)
        {
            PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, j);

            if (!prop->getterSymbol && !prop->setterSymbol)
            {
                DiagErrorFmt(diag, prop->range, "property '%s' must declare at least one of 'get' or 'set'",
                             prop->name);
                continue;
            }

            /* 'void' has no value to get/set; a dynamic `T[]` crosses extern
               as a fat {ptr,len} struct no C thunk can sanely implement for a
               single accessor. Fixed `T[N]` is rejected later by the
               function-level fixed-array rules (synthesized getter/setter). */
            if (strcmp(prop->returnType.name, "void") == 0)
            {
                DiagErrorFmt(diag, prop->range, "property '%s' may not have type 'void'", prop->name);
                continue;
            }

            if (TypeNameIsDynamicArray(&prop->returnType))
            {
                DiagErrorFmt(diag, prop->range,
                             "property '%s' may not have a dynamic array type ('%s')", prop->name,
                             prop->returnType.name);
                continue;
            }

            if (prop->getterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->getterSymbol, &prop->returnType, impl->handleName, false,
                                   prop->range, diag);
            }

            if (prop->setterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->setterSymbol, &prop->returnType, impl->handleName, true,
                                   prop->range, diag);
            }
        }
    }
}

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    /* `expr.Member(args)` / `Type.Static(args)` — resolve against impl blocks.
       On success the call is rewritten to the extern symbol (the receiver is
       prepended as the first argument for instance methods) and resolution
       falls through to the normal overload machinery. */
    if (c->calleeBase)
    {
        if (ResolveMemberCall(r, c, scope))
        {
            return;
        }
    }

    if (TypeRegistryIsUserType(&r->m_registry, c->callee))
    {
        /* Struct ctor: an owning field from an owning source is a move (same rules as var-decl). Deref checks run
         * before moves so the fact at call time is used. */
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            /* A braced arg against a struct field is a positional struct init; against an array field, an array
             * literal. */
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

            if (resolvedArg != arg)
            {
                VecSet(&c->args, j, resolvedArg);
            }
            else if (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
            {
                /* Array field from a braced list: infer element type and redo move-marking (skipped when first resolved
                 * with NULL type). */
                const TypeName* ft = &fd->type;
                const TypeName* ftArr = ft->isOptional ? ft->inner : ft;

                if (TypeNameIsDynamicArray(ftArr))
                {
                    const TypeName* elem = TypeNameArrayElem(ftArr);
                    ArrayInitExpr* ai = (ArrayInitExpr*)arg;

                    ai->elementType = elem;

                    if (elem && AliasIsOwning(r, elem))
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

            if (AliasIsOwning(r, &fd->type))
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

    // SIMD vector intrinsics: dot(a, b) and cross(a, b).
    if (c->callee != NULL && ResolveVectorIntrinsics(r, c, scope))
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

    // drop(string/^T/T[]): invalidate an owning value.
    if (c->callee != NULL && ResolveDropBuiltin(r, c, scope))
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
            size_t minArgs = functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1;

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
            bool isTail
                = functionDecl->isVariadic
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

            const ParamDecl* param
                = (ParamDecl*)VecGet(&functionDecl->params, isTail ? functionDecl->params.count - 1 : j);

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

                    optMatch = argInner != NULL ? strcmp(argInner->name, paramInner) == 0
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

    /* Ambiguity is already reported above; stop here so we don't run move-tracking /
       optional-deref / type-inference side effects against the (arbitrary) winning
       candidate and emit spurious cascading diagnostics. */
    if (ambiguous)
    {
        return;
    }

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

        /* Values that don't fit a non-negative signed i32 are 64-bit */
        if (lit->value > 0x7FFFFFFFULL)
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

            /* Box/optional operands deref to their inner for arithmetic
               (mirrors the ResolveExpr operand check). Same-shape SIMD
               vectors keep the vector type: the scalar ladder below would
               report a bogus `int` and break overload matching for valid
               vector arguments. */
            const char* ln2 = lt && lt->isBox && lt->inner ? lt->inner->name : ln;
            const char* rn2 = rt && rt->isBox && rt->inner ? rt->inner->name : rn;

            if (SameResolvedType(r, ln2, rn2) && IsSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0)
            {
                return InternTypeName(r, TypeRegistryResolveAlias(&r->m_registry, ln2));
            }

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

        /* Impl properties: a member read on a handle with a matching
           `property` yields the property's type. Methods are call-only
           (NULL here; the resolve pass diagnoses bare access). */
        PropertyDecl* prop = PropertyOnHandle(r, baseType, m->member);

        if (prop)
        {
            return &prop->returnType;
        }

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

        /* `string.length` (through aliases and `string?`): the fat length
           field, like T[] — ulong, matching arrays. */
        if (strcmp(m->member, "length") == 0
            && strcmp(TypeRegistryResolveAlias(&r->m_registry, baseType->name), "string") == 0)
        {
            return InternTypeName(r, "ulong");
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

        if (c->isPseudoCall && IsSimdVector(c->callee) != 0)
        {
            return InternTypeName(r, c->callee);
        }

        /* SIMD intrinsics: dot() reduces to scalar float; cross() keeps the arg vector type
           (cross(float2) reduces to the scalar z-component). */
        if (c->isPseudoCall && (strcmp(c->callee, "dot") == 0 || strcmp(c->callee, "cross") == 0))
        {
            Node* a0 = (Node*)VecGet(&c->args, 0);
            const TypeName* a0Type = InferType(r, a0, scope);

            const char* resolved0 = TypeRegistryResolveAlias(&r->m_registry, a0Type ? a0Type->name : "");
            if (strcmp(c->callee, "dot") == 0 || GetSimdVectorLanes(resolved0) == 2)
            {
                return InternTypeName(r, "float");
            }

            return a0Type;
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

        builtinType = DropBuiltinType(r, c, scope);

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

        if (baseType && strcmp(TypeRegistryResolveAlias(&r->m_registry, baseType->name), "string") == 0)
        {
            /* `s[i]` yields the byte at i (bounds-checked in codegen). */
            return InternTypeName(r, "byte");
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

        /* `T?` accepts `^T` (and vice versa) when inners match (same ABI). The reverse needs a narrowing fact (checked
         * by callers). */
        const TypeName* valueInner = TypeNameBoxInner(valueType);

        if (targetType->isOptional && valueType->isBox && inner && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        return (inner && strcmp(inner->name, valueType->name) == 0) || strcmp(valueType->name, targetType->name) == 0;
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

    // Assignment to a SIMD vector: the lane counts must match exactly; a scalar splat-broadcasts
    // into any vector. A non-vector never lands in a vector slot.

    const char* resolvedTarget = TypeRegistryResolveAlias(&r->m_registry, targetType->name);
    const char* resolvedValue = TypeRegistryResolveAlias(&r->m_registry, valueType->name);
    const int isTargetVector = IsSimdVector(resolvedTarget);
    const int isValueVector = IsSimdVector(resolvedValue);

    if (isTargetVector && isValueVector && isTargetVector == isValueVector)
    {
        /* vector = same-lane vector — only when both are the same type (raw or same alias). */
        if (strcmp(targetType->name, valueType->name) == 0)
        {
            return true;
        }

        /* Both resolve to the same raw SIMD type but have different names — that means
           they are different type aliases of the same underlying SIMD type.  Reject
           (no implicit conversion between distinct alias types). */
    }

    if (isTargetVector && isValueVector == 0 && IsNumeric(resolvedValue))
    {
        /* vector = scalar splat — only when the target is a raw SIMD type, not an alias. */
        if (!TypeRegistryIsTypeAlias(&r->m_registry, targetType->name))
        {
            return true;
        }
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
        else if (AliasIsOwning(r, varType))
        {
            /* Whole-value use: reject if fully or partially moved. Member-base use: reject only if fully moved (descend
             * into partial). */
            bool blocked = asMemberBase ? IsBoxMoved(r, ident->name) : IsBoxUnusable(r, ident->name);

            if (blocked)
            {
                if (IsBoxPartiallyMoved(r, ident->name))
                {
                    DiagErrorFmt(r->m_diag, ident->base.range, "'%s' is poisoned", ident->name);
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

        /* Arithmetic/bitwise operators need numeric operands, or two vectors
           of the same shape (raw or through a shared alias). Anything else
           used to fall through to a bogus `int` inference and lower to
           invalid pointer arithmetic. */
        switch (b->op)
        {
        case BinAdd:
        case BinSub:
        case BinMul:
        case BinDiv:
        case BinMod:
        case BinBitAnd:
        case BinBitOr:
        case BinBitXor:
        case BinShl:
        case BinShr:
        {
            const TypeName* lt = InferType(r, b->lhs, scope);
            const TypeName* rt = InferType(r, b->rhs, scope);
            const char* ln = lt ? lt->name : "";
            const char* rn = rt ? rt->name : "";

            /* An unknown operand type (inference returns NULL for some
               extern-struct member/index chains) is not proof of invalidity.
               A box/optional operand derefs to its inner for arithmetic. */
            const char* ln2 = lt && lt->isBox && lt->inner ? lt->inner->name : ln;
            const char* rn2 = rt && rt->isBox && rt->inner ? rt->inner->name : rn;

            bool numericPair = IsNumeric(ln2) && IsNumeric(rn2);
            bool vectorPair = SameResolvedType(r, ln2, rn2)
                              && IsSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0;

            if (lt && rt && !numericPair && !vectorPair)
            {
                DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
            }

            break;
        }
        default:
            break;
        }

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

        /* Property write (`cam.FOV = v`): lowered to the setter extern call.
           Compound assignment is rejected, read-only properties error, and
           the value must be assignable to the property type. */
        if (a->target->kind == NodeMember)
        {
            MemberExpr* tm = (MemberExpr*)a->target;
            PropertyDecl* prop = PropertyOnHandle(r, InferType(r, tm->base_node, scope), tm->member);

            if (prop)
            {
                const TypeName* rawBase = InferType(r, tm->base_node, scope);

                if (rawBase && rawBase->isOptional)
                {
                    const char* baseKey = MovableBoxSourceKey(r, tm->base_node);

                    if (!IsPathNonEmpty(r, baseKey))
                    {
                        DiagOptionalReadError(r, a->base.range, baseKey, rawBase->name);
                    }
                }

                ResolveExpr(r, tm->base_node, scope);
                ResolveExpr(r, a->value, scope);

                /* The setter call produces no value: `a.P = b.Q = v` has no
                   meaningful result (and an inner write used as a value would
                   flow a void into the outer setter). */
                if (a->value->kind == NodeAssign && ((AssignExpr*)a->value)->isImplPropertyWrite)
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "cannot chain property assignment; assign each property separately");
                    return;
                }

                a->isImplPropertyWrite = true;

                if (a->op != AssignSet)
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "cannot compound-assign to property '%s'; assign it directly", tm->member);
                    return;
                }

                if (!prop->setterSymbol)
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "property '%s' is read-only", tm->member);
                    return;
                }

                const TypeName* vt = InferType(r, a->value, scope);

                if (vt && !IsAssignableType(r, &prop->returnType, vt))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to property '%s' of type '%s'",
                                 vt->name, tm->member, prop->returnType.name);
                }

                return;
            }
        }

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
        bool targetIsBox = tt && AliasIsOwning(r, tt);

        /* An owning field target is reassigned, not read - re-life it instead of tripping use-after-move. */
        const char* fieldKey = NULL;
        bool targetIsOwningField = false;

        if (a->target->kind == NodeMember)
        {
            const TypeName* ft = InferType(r, a->target, scope);

            if (ft && AliasIsOwning(r, ft))
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

            /* `=` rebinds the box; other assigns mutate contents. An optional target is always rebound (compound ops
             * rejected). */
            bool optionalTarget = tt->isOptional;

            if (optionalTarget && a->op != AssignSet)
            {
                DiagErrorFmt(r->m_diag, a->base.range,
                             "cannot compound-assign into '%s' of nullable type '%s'; it has not been blessed",
                             MovableBoxSourceKey(r, a->target), tt->name);

                return;
            }

            bool boxMove = a->op == AssignSet && vt && (strcmp(vt->name, tt->name) == 0 || optionalTarget);
            const char* targetName = MovableBoxSourceKey(r, a->target);

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

                /* The rebind drops the target's blessing; a plain `=` proves it non-empty again only when the new
                 * value does - a non-optional source, or an optional path already blessed (checked before
                 * MarkBoxLive clears the target subtree's facts). */
                const char* movedValueKey = MovableBoxSourceKey(r, a->value);
                bool valueProvesNonEmpty = !optionalTarget || (vt && !vt->isOptional)
                                           || (movedValueKey && IsPathNonEmpty(r, movedValueKey));

                MarkBoxLive(r, targetName);

                if (valueProvesNonEmpty)
                {
                    MarkPathNonEmpty(r, targetName);
                }

                if (movedValueKey)
                {
                    if (vt && vt->isOptional)
                    {
                        /* Moving from a `T?` leaves the source empty, never poisoned. */
                        MoveOptionalSource(r, movedValueKey, a->base.range);
                    }
                    else
                    {
                        MoveBoxIdent(r, movedValueKey, a->base.range);
                    }
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
                root = (root->kind == NodeMember) ? ((MemberExpr*)root)->base_node : ((IndexExpr*)root)->base_node;
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
                const TypeName* vt = InferType(r, a->value, scope);
                const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                /* The rebind drops the field's stale facts; it is provably non-empty again only when the assigned
                 * value is (checked before ClearNullableFacts wipes the subtree). */
                bool valueProvesNonEmpty = !vt || !vt->isOptional
                                           || (movedValueKey && IsPathNonEmpty(r, movedValueKey));

                ClearNullableFacts(r, fieldKey);

                if (valueProvesNonEmpty)
                {
                    MarkPathNonEmpty(r, fieldKey);
                }

                /* If the RHS is itself an owning binding, it moves out of
                    that source - same as a box-to-box assignment; an optional
                    source is left empty, never poisoned. */
                if (movedValueKey)
                {
                    if (vt && vt->isOptional)
                    {
                        MoveOptionalSource(r, movedValueKey, a->base.range);
                    }
                    else
                    {
                        MoveBoxIdent(r, movedValueKey, a->base.range);
                    }
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

        /* Properties have no addressable storage: ++/-- would need a
           get+set pair; require explicit assignment instead. */
        if (inc->operand->kind == NodeMember)
        {
            MemberExpr* tm = (MemberExpr*)inc->operand;

            if (PropertyOnHandle(r, InferType(r, tm->base_node, scope), tm->member))
            {
                DiagErrorFmt(r->m_diag, inc->base.range,
                             "cannot %s property '%s'; assign it explicitly instead", inc->isDec ? "decrement" : "increment",
                             tm->member);
                return;
            }
        }

        CheckConstAssign(r, inc->operand, inc->base.range);
        ResolveExpr(r, inc->operand, scope);

        /* ++/-- only makes sense on numeric storage; anything else would
           fall through to pointer arithmetic. */
        const TypeName* incType = InferType(r, inc->operand, scope);

        if (incType && !IsNumeric(incType->name))
        {
            DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s a value of type '%s' (expected a numeric type)",
                         inc->isDec ? "decrement" : "increment", incType->name);
        }

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

        /* Resolve type aliases to their underlying types for cast compatibility. */
        const char* resolvedSrc = TypeRegistryResolveAlias(&r->m_registry, srcName);
        const char* resolvedDst = TypeRegistryResolveAlias(&r->m_registry, dstName);

        bool scalarPair = src && (IsScalarLikeType(&r->m_registry, srcName) && IsScalarLikeType(&r->m_registry, dstName));
        bool handlePair = src && IsHandleType(&r->m_registry, srcName) && IsHandleType(&r->m_registry, dstName)
                          && (HandleExtendsFrom(&r->m_registry, dstName, srcName)
                              || HandleExtendsFrom(&r->m_registry, srcName, dstName));

        /* SIMD vector pair: same lane count (including through type aliases). */
        const int srcLanes = IsSimdVector(resolvedSrc);
        const int dstLanes = IsSimdVector(resolvedDst);
        bool simdPair = src && srcLanes != 0 && dstLanes != 0 && srcLanes == dstLanes;

        /* ^T -> ^U only when T or U is opaque (erase/cast-back). */
        bool boxPair = src && TypeNameIsOwning(src) && TypeNameIsOwning(dst)
                       && ((TypeNameBoxInner(src) && TypeRegistryIsOpaque(&r->m_registry, src->inner->name))
                           || (TypeNameBoxInner(dst) && TypeRegistryIsOpaque(&r->m_registry, dst->inner->name)));

        /* Alias <-> underlying (or alias <-> alias) of the SAME resolved type:
           the explicit escape hatch for strong typedefs (`(Name)s`, `(string)n`).
           Distinct aliases still never convert implicitly. */
        bool aliasPair = src && SameResolvedType(r, srcName, dstName)
                         && (TypeRegistryIsTypeAlias(&r->m_registry, srcName)
                             || TypeRegistryIsTypeAlias(&r->m_registry, dstName));

        if (src && !scalarPair && !handlePair && !simdPair && !boxPair && !aliasPair)
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

        /* Impl property reads are call-like: no lvalue, no move source. */
        m->isImplProperty = PropertyOnHandle(r, rawBaseType, m->member) != NULL;

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
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'",
                             baseType->name);
            }
            else if (IsHandleType(&r->m_registry, baseType->name))
            {
                PropertyDecl* prop = PropertyOnHandle(r, rawBaseType, m->member);

                if (prop)
                {
                    if (!prop->getterSymbol)
                    {
                        DiagErrorFmt(r->m_diag, m->base.range, "property '%s' is write-only", m->member);
                    }
                }
                else if (FindImplMethod(r, baseType->name, m->member))
                {
                    DiagErrorFmt(r->m_diag, m->base.range, "method '%s' must be called: use '%s.%s(...)'", m->member,
                                 baseType->name, m->member);
                }
                else
                {
                    DiagErrorFmt(r->m_diag, m->base.range, "handle '%s' has no member '%s'", baseType->name,
                                 m->member);
                }
            }
            else
            {
                DiagError(r->m_diag, m->base.range, "cannot access a member of an opaque handle");
            }
        }

        /* Box fields can be moved out too; IsBoxUnusable catches full or partial moves. */
        const TypeName* selfType = InferType(r, n, scope);

        if (selfType && AliasIsOwning(r, selfType))
        {
            const char* key = MovableBoxSourceKey(r, n);

            if (key && IsBoxUnusable(r, key))
            {
                if (IsBoxPartiallyMoved(r, key))
                {
                    DiagErrorFmt(r->m_diag, m->base.range, "'%s' is poisoned", key);
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
        else if (TypeRegistryIsTypeAlias(&r->m_registry, structInitExpr->typeName))
        {
            DiagErrorFmt(r->m_diag, structInitExpr->base.range,
                         "'%s' is a type alias and cannot be initialized with a struct literal",
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
                        /* Fixed array: shape must mirror type - nested rows per dimension, flat list for 1-D; rows
                         * place row-major. */
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
                                continue; /* hole: stays zero */
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
                    = (AliasIsOwning(r, &fieldDecl->type) && fieldValueType2 && AliasIsOwning(r, fieldValueType2))
                          ? MovableBoxSourceKey(r, field->value)
                          : NULL;

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
            size_t fieldCount = structType->fields.count;
            bool* covered = (bool*)arena_alloc(r->m_arena, fieldCount * sizeof(bool));

            for (size_t i = 0; i < structInitExpr->fields.count; i++)
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

                if (idx < fieldCount)
                {
                    covered[idx] = true;
                }
            }

            for (size_t f = 0; f < fieldCount; f++)
            {
                FieldDecl* fd = (FieldDecl*)VecGet(&structType->fields, f);

                /* Every OWNING field (`^T`, owning struct, `string`) must be
                   initialized; optionals and dynamic arrays may stay empty
                   (a T? is null, a zero-filled T[] is the canonical empty
                   {null, 0} fat struct). */
                bool mustInit
                    = !fd->type.isOptional && !TypeNameIsDynamicArray(&fd->type)
                      && (AliasIsOwning(r, &fd->type) || TypeRegistryIsOwningStruct(&r->m_registry, fd->type.name));

                if (mustInit && !covered[f])
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
                         "'?' test requires a nullable type ('T?'), but '%s' can never be empty", operandType->name);
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
            if (ai->elementType && AliasIsOwning(r, ai->elementType))
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

/* A loop body repeats, so a move must stay valid across the back edge. Walk once muted (warmup) to carry state forward,
 * then walk again for real; errors needing carried-over state surface on the second pass. */
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

        /* An unknown type must be reported at sema time so codegen is skipped
           (otherwise the backend keeps lowering it and corrupts). Walk through
           box/optional/array wrappers to the leaf type name. */
        {
            const TypeName* t = &vd->type;
            while (t && (t->isBox || t->isOptional || t->isArray))
            {
                t = (t->isBox || t->isOptional) ? t->inner : t->elem;
            }
            if (t && t->name && strcmp(t->name, "string") != 0 && !IsScalarTypeName(t->name)
                && !IsSimdVector(t->name) && !TypeRegistryIsUserType(&r->m_registry, t->name))
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "unknown type '%s'", t->name);
                return;
            }
        }

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
        if (AliasIsOwning(r, &vd->type) && !TypeNameIsDynamicArray(&vd->type) && !TypeNameIsOptional(&vd->type)
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
                DiagErrorFmt(r->m_diag, vd->base.range, "owning struct '%s' must be stored in a box; use '^%s[]'",
                             arrElem->name, arrElem->name);
            }
        }

        bool initProvesNonEmpty = true;

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
            const char* movedInitKey = initType && AliasIsOwning(r, &vd->type) && AliasIsOwning(r, initType)
                                           ? MovableBoxSourceKey(r, vd->init)
                                           : NULL;

            /* An optional-typed initializer proves the binding non-empty only when it is itself blessed (checked
             * before the move below clears that fact). */
            if (vd->type.isOptional)
            {
                initProvesNonEmpty = (initType && !initType->isOptional)
                                     || (movedInitKey && IsPathNonEmpty(r, movedInitKey));
            }

            if (movedInitKey)
            {
                if (initType->isOptional)
                {
                    /* Moving from a `T?` leaves the source empty, never poisoned. */
                    MoveOptionalSource(r, movedInitKey, vd->base.range);
                }
                else
                {
                    MoveBoxIdent(r, movedInitKey, vd->base.range);
                }
            }
        }

        /* Fresh binding: clear stale moved-state, nullable facts, and index deps from any prior same-named var. */
        ClearBoxSubtree(r, vd->name);
        ClearNullableFacts(r, vd->name);
        InvalidateIndexVar(r, vd->name);

        /* An initialized declaration proves the binding non-empty - unless initialized from a maybe-empty `T?`. */
        if (vd->init && initProvesNonEmpty)
        {
            MarkPathNonEmpty(r, vd->name);
        }

        StrMapPut(scope, vd->name, (void*)&vd->type);

        return;
    }
    case NodeExprStmt:
        ResolveExpr(r, ((ExprStmt*)n)->expr, scope);
        return;
    case NodeDefer:
    {
        DeferStmt* d = (DeferStmt*)n;
        /* Analyze the deferred statement at its textual position (move/const
           checks apply here); it is executed later, at enclosing-block exit. */
        WalkStmt(r, d->stmt, scope);
        return;
    }
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
            const char* movedReturnKey
                = typeName && AliasIsOwning(r, typeName) ? MovableBoxSourceKey(r, rs->value) : NULL;

            if (movedReturnKey)
            {
                bool returnsSameBox
                    = r->m_currentReturnType && strcmp(r->m_currentReturnType->name, typeName->name) == 0;

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
                    bool innerMatchesReturn
                        = boxInnerName
                          && (r->m_currentReturnType
                              && (strcmp(r->m_currentReturnType->name, boxInnerName) == 0
                                  || (IsNumeric(boxInnerName) && IsNumeric(r->m_currentReturnType->name))));

                    if (innerIsOwning || !innerMatchesReturn)
                    {
                        MoveBoxIdent(r, movedReturnKey, rs->base.range);
                    }
                }
            }
        }
        else if (r->m_currentReturnType && strcmp(r->m_currentReturnType->name, "void") != 0)
        {
            DiagErrorFmt(r->m_diag, rs->base.range, "non-void function must return a value");
        }

        return;
    }
    case NodeIf:
    {
        IfStmt* i = (IfStmt*)n;
        ResolveExpr(r, i->condition, scope);

        /* `if (path?)` blesses the then-branch; `if (!path?)` blesses the else. Facts intersect at the join;
         * moved-state unions. */
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
        StrMapFree(&beforeEmpty);

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

/* ---- Missing-return flow analysis ---- */

static bool ExprIsConstantTrue(const Node* n);
static bool ExprIsConstantFalse(const Node* n);

static bool ExprIsConstantTrue(const Node* n)
{
    if (!n)
    {
        return false;
    }
    switch (n->kind)
    {
    case NodeIntLiteral:
        return ((const IntLiteral*)n)->value != 0;
    case NodeBoolLiteral:
        return ((const BoolLiteral*)n)->value;
    case NodeUnary:
        if (((const UnaryExpr*)n)->op == UnNot)
        {
            return ExprIsConstantFalse(((const UnaryExpr*)n)->operand);
        }
        return false;
    default:
        return false;
    }
}

static bool ExprIsConstantFalse(const Node* n)
{
    if (!n)
    {
        return false;
    }
    switch (n->kind)
    {
    case NodeIntLiteral:
        return ((const IntLiteral*)n)->value == 0;
    case NodeBoolLiteral:
        return !((const BoolLiteral*)n)->value;
    case NodeUnary:
        if (((const UnaryExpr*)n)->op == UnNot)
        {
            return ExprIsConstantTrue(((const UnaryExpr*)n)->operand);
        }
        return false;
    default:
        return false;
    }
}

/* True if a `break` targeting the loop at `targetDepth` can be reached on some
   path through `n`, where `depth` is the current loop-nesting depth. A `break`
   inside a nested loop targets that loop, not ours. */
static bool BodyCanBreak(const Node* n, int depth, int targetDepth)
{
    if (!n)
    {
        return false;
    }

    switch (n->kind)
    {
    case NodeBreak:
        return depth == targetDepth;
    case NodeBlock:
    {
        const Block* b = (const Block*)n;
        for (size_t i = 0; i < b->statements.count; i++)
        {
            if (BodyCanBreak((const Node*)VecGet(&b->statements, i), depth, targetDepth))
            {
                return true;
            }
        }
        return false;
    }
    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        return BodyCanBreak(s->thenBranch, depth, targetDepth) || BodyCanBreak(s->elseBranch, depth, targetDepth);
    }
    case NodeWhile:
        return BodyCanBreak(((const WhileStmt*)n)->body, depth + 1, targetDepth);
    case NodeFor:
        return BodyCanBreak(((const ForStmt*)n)->body, depth + 1, targetDepth);
    default:
        return false;
    }
}

/* True if control can reach the statement after `n` (fall off its end) on some
   path. `depth` is the current loop-nesting depth. */
static bool StmtFallsThrough(const Node* n, int depth)
{
    if (!n)
    {
        return true;
    }

    switch (n->kind)
    {
    case NodeReturn:
    case NodeBreak:
    case NodeContinue:
        return false;

    case NodeBlock:
    {
        const Block* b = (const Block*)n;
        for (size_t i = 0; i < b->statements.count; i++)
        {
            if (!StmtFallsThrough((const Node*)VecGet(&b->statements, i), depth))
            {
                return false; /* terminating statement hit; rest is dead */
            }
        }
        return true;
    }

    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        if (ExprIsConstantTrue(s->condition))
        {
            return StmtFallsThrough(s->thenBranch, depth);
        }
        if (ExprIsConstantFalse(s->condition))
        {
            return StmtFallsThrough(s->elseBranch, depth);
        }
        if (!s->elseBranch)
        {
            return true; /* false branch always falls through */
        }
        return StmtFallsThrough(s->thenBranch, depth) || StmtFallsThrough(s->elseBranch, depth);
    }

    case NodeWhile:
    {
        const WhileStmt* s = (const WhileStmt*)n;
        if (ExprIsConstantTrue(s->condition))
        {
            /* Infinite unless the body breaks out. */
            return BodyCanBreak(s->body, depth + 1, depth + 1);
        }
        /* May run zero times -> control reaches after the loop. */
        return true;
    }

    case NodeFor:
    {
        const ForStmt* s = (const ForStmt*)n;
        if (!s->condition || ExprIsConstantTrue(s->condition))
        {
            /* for(;;) or for(;true;) : infinite unless body breaks. */
            return BodyCanBreak(s->body, depth + 1, depth + 1);
        }
        return true;
    }

    default:
        return true;
    }
}

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Resolver r = {0};
    r.m_mod = mod;
    r.m_diag = diag;
    r.m_arena = arena;

    TypeRegistryInit(&r.m_registry);

    /* Manifest constants: fold `const` scalar global initializers FIRST, so
       `[constName]` fixed-array dimensions resolve before layouts compute.
       Aliases register first — a `const AliasType` folds through to its
       underlying scalar. */
    TypeRegistryRegisterAliases(&r.m_registry, mod);
    StrMapInit(&r.m_constGlobals);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, i);

        if (!gd->type.isConst || !gd->init || !IsManifestConstType(&r.m_registry, gd->type.name))
        {
            continue;
        }

        ConstGlobalVal* v = (ConstGlobalVal*)arena_alloc(arena, sizeof(ConstGlobalVal));
        v->isInt = IsConstDimType(&r.m_registry, gd->type.name);

        if (SemaFoldConstInit(&r, gd->init, v))
        {
            StrMapPut(&r.m_constGlobals, gd->name, v);
        }
    }

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

            SemaResolveConstDims(&r, &field->type);
        }
    }

    TypeRegistryBuild(&r.m_registry, mod);

    /* Validate impl targets and declare property accessor externs before the
       overload/mangling pass runs. */
    ResolveImpls(mod, diag, arena, &r.m_registry);

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

        if (AliasIsOwning(&r, &gd->type))
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
        }

        ResetStrMap(&r.m_constVars);
        ResetStrMap(&r.m_movedBoxes);
        ResetStrMap(&r.m_refBoxParams);

        for (size_t j = 0; j < mod->globals.count; j++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&mod->globals, j);

            if (gd->type.isConst)
            {
                StrMapPut(&r.m_constVars, gd->name, (void*)1);
            }
        }

        for (size_t j = 0; j < functionDecl->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&functionDecl->params, j);
            StrMapPut(&scope, p->name, (void*)&p->type);

            if (p->type.isConst)
            {
                StrMapPut(&r.m_constVars, p->name, (void*)1);
            }

            if (AliasIsOwning(&r, &p->type) && p->mod == ModRef)
            {
                StrMapPut(&r.m_refBoxParams, p->name, (void*)1);
            }
        }

        r.m_currentReturnType = &functionDecl->returnType;

        WalkBlock(&r, (Block*)functionDecl->body, &scope);
        r.m_currentReturnType = NULL;

        /* A non-void function must return on every path; if control can fall
           off the end of the body, at least one path is missing a return. */
        if (strcmp(functionDecl->returnType.name, "void") != 0 && StmtFallsThrough(functionDecl->body, 0))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "missing return statement in function '%s' returning '%s'",
                         functionDecl->name, functionDecl->returnType.name);
        }

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

            if (sd->isExtern
                && strcmp(TypeRegistryResolveAlias(&r.m_registry, field->type.name), "string") == 0)
            {
                /* A string is a fat {ptr, len} pair internally; an extern
                   struct must mirror the host's C layout byte-for-byte, so a
                   char* member has no string spelling. */
                DiagErrorFmt(diag, field->type.range,
                             "extern struct field '%s' may not have type 'string' "
                             "(use a '^byte' or integer-typed member for a raw char*)",
                             field->name);
            }

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
                    DiagErrorFmt(diag, field->type.range, "fixed-size array field '%s' may not contain a dynamic array",
                                 field->name);
                }
                else if (leaf->isBox || AliasIsOwning(&r, leaf))
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
                    ResolveExpr(&r, gd->init, &globalScope);
                    const TypeName* initType = InferType(&r, gd->init, &globalScope);

                    if (initType && strcmp(initType->name, gd->type.name) != 0)
                    {
                        DiagErrorFmt(diag, gd->base.range,
                                     "global '%s' of type '%s' cannot be initialized by expression of type '%s'",
                                     gd->name, gd->type.name, initType->name);
                    }
                }

                continue;
            }

            /* Scalar / alias / struct globals: the initializer is type-checked
               like a local declaration (the backend only lowers compile-time
               constant initializers, so nothing here may slip through). */
            if (!AliasIsOwning(&r, &gd->type))
            {
                if (gd->init)
                {
                    ResolveExpr(&r, gd->init, &globalScope);
                    const TypeName* initType = InferType(&r, gd->init, &globalScope);

                    if (initType && !IsAssignableType(&r, &gd->type, initType))
                    {
                        DiagErrorFmt(diag, gd->base.range,
                                     "global '%s' of type '%s' cannot be initialized by expression of type '%s'",
                                     gd->name, gd->type.name, initType->name);
                    }
                }

                continue;
            }

            /* string / alias-of-string / string? globals default to the
                canonical empty {null, 0} fat - no init required, exactly
                like arrays. An initializer is still type-checked below. */
            bool stringLike
                = strcmp(TypeRegistryResolveAlias(&r.m_registry, gd->type.name), "string") == 0
                  || (gd->type.isOptional && gd->type.inner
                      && strcmp(TypeRegistryResolveAlias(&r.m_registry, gd->type.inner->name), "string") == 0);

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

        /* Resolve aliases first: `struct Name = string;` returns a scalar
           pointer across the boundary, not a struct. */
        if (functionDecl->isExtern
            && IsDefinedStruct(&r.m_registry,
                               TypeRegistryResolveAlias(&r.m_registry, functionDecl->returnType.name)))
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
    StrMapFree(&r.m_constGlobals);
    StrMapFree(&r.m_movedBoxes);
    StrMapFree(&r.m_nonEmptyPaths);
    StrMapFree(&r.m_indexDeps);
    StrMapFree(&r.m_boxGlobals);
    StrMapFree(&r.m_refBoxParams);
    StrMapFree(&r.m_typeCache);
    StrMapFree(&byMangled);
    TypeRegistryFree(&r.m_registry);
}
