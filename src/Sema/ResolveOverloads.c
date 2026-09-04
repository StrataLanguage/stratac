#include "Sema/ResolveOverloads.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"

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

/* The scalar integral types an enum may be based on. */
static bool IsEnumUnderlyingType(const char* t)
{
    return strcmp(t, "int") == 0 || strcmp(t, "uint") == 0 || strcmp(t, "long") == 0 || strcmp(t, "ulong") == 0
           || strcmp(t, "byte") == 0 || strcmp(t, "sbyte") == 0 || strcmp(t, "short") == 0 || strcmp(t, "ushort") == 0;
}

/* Signed bounds for the enum underlying types. */
static void EnumSignedRange(const char* underlying, bool* isUnsigned, int64_t* minVal, uint64_t* maxVal)
{
    if (strcmp(underlying, "byte") == 0)
    {
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = UCHAR_MAX;
    }
    else if (strcmp(underlying, "ushort") == 0)
    {
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = USHRT_MAX;
    }
    else if (strcmp(underlying, "uint") == 0)
    {
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = UINT_MAX;
    }
    else if (strcmp(underlying, "ulong") == 0)
    {
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = ULLONG_MAX;
    }
    else if (strcmp(underlying, "sbyte") == 0)
    {
        *isUnsigned = false;
        *minVal = SCHAR_MIN;
        *maxVal = (uint64_t)SCHAR_MAX;
    }
    else if (strcmp(underlying, "short") == 0)
    {
        *isUnsigned = false;
        *minVal = SHRT_MIN;
        *maxVal = (uint64_t)SHRT_MAX;
    }
    else if (strcmp(underlying, "int") == 0)
    {
        *isUnsigned = false;
        *minVal = INT_MIN;
        *maxVal = (uint64_t)INT_MAX;
    }
    else /* "long" */
    {
        *isUnsigned = false;
        *minVal = LLONG_MIN;
        *maxVal = (uint64_t)LLONG_MAX;
    }
}

/* Interprets `mag` (with sign) as a signed value; false when the magnitude
   cannot be represented in int64 (only reachable for `long` underlying). */
static bool EnumSignedValue(uint64_t mag, bool isNegative, int64_t* out)
{
    if (isNegative)
    {
        if (mag == (1ULL << 63))
        {
            *out = INT64_MIN;
            return true;
        }

        if (mag > (uint64_t)INT64_MAX)
        {
            return false;
        }

        *out = -(int64_t)mag;
        return true;
    }

    if (mag > (uint64_t)INT64_MAX)
    {
        return false;
    }

    *out = (int64_t)mag;
    return true;
}

/* Extracts a compile-time integer value from a RESOLVED constant expression:
   an integer literal, a +/- literal, or an enum constant. */
static bool ResolvedConstIntValue(Node* n, uint64_t* mag, bool* isNegative)
{
    *mag = 0;
    *isNegative = false;

    if (n->kind == NodeIntLiteral)
    {
        *mag = ((IntLiteral*)n)->value;
        return true;
    }

    if (n->kind == NodeMember && ((MemberExpr*)n)->isEnumConst)
    {
        *mag = ((MemberExpr*)n)->enumValue;
        return true;
    }

    /* `int.max` / `float.min`: integral pseudo-properties fold to their
       bit pattern (floats are not integer constants). */
    if (n->kind == NodeMember && ((MemberExpr*)n)->isScalarConst)
    {
        MemberExpr* m = (MemberExpr*)n;
        uint64_t intVal = 0;
        bool isFloat = false;

        if (m->base_node->kind == NodeIdent
            && ScalarPseudoConst(((IdentExpr*)m->base_node)->name, m->member, &intVal, NULL, &isFloat) && !isFloat)
        {
            *mag = intVal;
            return true;
        }

        return false;
    }

    if (n->kind == NodeUnary)
    {
        UnaryExpr* u = (UnaryExpr*)n;

        if (u->op == UnNeg)
        {
            if (u->operand->kind == NodeIntLiteral)
            {
                *mag = ((IntLiteral*)u->operand)->value;
                *isNegative = true;
                return true;
            }

            if (u->operand->kind == NodeMember && ((MemberExpr*)u->operand)->isEnumConst)
            {
                *mag = ((MemberExpr*)u->operand)->enumValue;
                *isNegative = true;
                return true;
            }
        }
        else if (u->op == UnPos && u->operand->kind == NodeIntLiteral)
        {
            *mag = ((IntLiteral*)u->operand)->value;
            return true;
        }
    }

    return false;
}

/* Range-checks a constant value against a scalar integral type name. */
static bool EnumConstFits(const char* underlying, uint64_t mag, bool isNegative)
{
    bool isUnsigned = false;
    int64_t minVal = 0;
    uint64_t maxVal = 0;
    EnumSignedRange(underlying, &isUnsigned, &minVal, &maxVal);

    if (isUnsigned)
    {
        return !isNegative && mag <= maxVal;
    }

    int64_t sv = 0;
    return EnumSignedValue(mag, isNegative, &sv) && sv >= minVal && (uint64_t)sv <= maxVal;
}

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
    case NodeMember:
    {
        /* `int.max` / `float.min` — builtin scalar pseudo-properties fold
           to their constants, exactly like enum constants. */
        MemberExpr* m = (MemberExpr*)n;

        if (m->base_node->kind != NodeIdent)
        {
            return false;
        }

        uint64_t intVal = 0;
        double floatVal = 0.0;
        bool isFloat = false;

        if (ScalarPseudoConst(((IdentExpr*)m->base_node)->name, m->member, &intVal, &floatVal, &isFloat))
        {
            bool wasInt = out->isInt;
            out->i = (long long)intVal;
            out->f = floatVal;
            out->isFloat = isFloat;
            out->isInt = wasInt;
            return true;
        }

        /* `EnumName.Member` — a scoped enum constant folds to its value. Runs
           BEFORE the main resolution marks members, so scan the module. */
        if (!TypeRegistryIsEnum(&r->m_registry, ((IdentExpr*)m->base_node)->name))
        {
            return false;
        }

        const char* baseName = ((IdentExpr*)m->base_node)->name;

        for (size_t i = 0; i < r->m_mod->enums.count; i++)
        {
            EnumDecl* ed = (EnumDecl*)VecGet(&r->m_mod->enums, i);

            if (strcmp(ed->name, baseName) != 0)
            {
                continue;
            }

            for (size_t j = 0; j < ed->members.count; j++)
            {
                EnumMemberDecl* member = (EnumMemberDecl*)VecGet(&ed->members, j);

                if (strcmp(member->name, m->member) == 0)
                {
                    bool wasInt = out->isInt;
                    out->i = (long long)member->value;
                    out->f = 0.0;
                    out->isFloat = false;
                    out->isInt = wasInt;
                    return true;
                }
            }

            return false;
        }

        return false;
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

/* Count of params a call actually passes arguments for: excludes the
   trailing typed-rest param and the implicit `return` out-param of externs. */
static size_t NamedParamCount(const FunctionDecl* f)
{
    bool typedRest = f->isVariadic && !f->isCVararg;
    size_t n = typedRest && f->params.count > 0 ? f->params.count - 1 : f->params.count;

    if (FunctionHasReturnParam(f))
    {
        n -= 1;
    }

    return n;
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

// Fill `flat` with fixed-array init leaves in row-major order.
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

/* Owning when the type itself owns, or its alias resolves to one. */
static bool AliasIsOwning(const Resolver* r, const TypeName* t)
{
    return TypeIsOwningResolved(&r->m_registry, r->m_arena, t);
}

/* True when the type is a dynamic array, or its alias resolves to one. */
static bool ResolvesToDynamicArray(const Resolver* r, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (TypeNameIsDynamicArray(t))
    {
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->m_registry, t->name);

    if (!leaf || strcmp(leaf, t->name) == 0)
    {
        return false;
    }

    TypeName parsed = TypeNameParse(r->m_arena, leaf);
    return TypeNameIsDynamicArray(&parsed);
}

/* True when both names resolve to the same underlying type.
   Distinct aliases never unify implicitly; explicit casts use this. */
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

// A descendant field moved out: fields stay readable, whole use does not.
static bool IsBoxPartiallyMoved(const Resolver* r, const char* name)
{
    return StrMapGet(&r->m_movedBoxes, name) == (void*)3;
}

// True when `child` is `parent` or nested under it.
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

    // Reassigning re-lives the binding; the caller re-blesses it as needed.
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

// Root binding of a move key ("holder.gun" -> "holder").
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

// Mark every dotted/bracketed prefix of `key` partially moved.
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

// Non-empty (`T?`) facts: a path maps to 1 while proven non-empty.
static bool IsPathNonEmpty(const Resolver* r, const char* key);
static void MarkPathNonEmpty(Resolver* r, const char* key);
static void ClearNonEmptySubtree(Resolver* r, const char* key);

static bool IsPathNonEmpty(const Resolver* r, const char* key)
{
    return key && StrMapGet(&r->m_nonEmptyPaths, key) == (void*)1;
}

// True when `key` uses the erased "a[]" form (never provable).
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

// Remember which facts mention each index variable.
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

// Drop facts indexed by `var` (assigned, inc/dec, or non-const ref).
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

// Error for reading an unproven optional; erased keys suggest a local.
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

// Unwrapping an optional into a plain target needs a proven-non-empty fact.
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

// Operand of `path?`/`!path?`; sets *negated for the `!` form.
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

// Keep only facts proven on both branches.
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

// Mark `name` moved, unless it is a borrowed global or ref param.
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
    ClearNonEmptySubtree(r, name); // Moving out clears the fact.
}

// Moving a `T?` empties the source instead of poisoning it.
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

// Canonical index spelling for path keys; NULL when not trackable.
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

        // Globals have no visible mutation site: erase.
        return IsModuleGlobalName(r, name) ? NULL : arena_format(r->m_arena, "%s", name);
    }

    case NodeMember:
    {
        // `.length` of a local only.
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
        // Nested index: spell both sides.
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
            return NULL; // `!` yields a boolean, never an index.
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

        // Precise index when possible; else the erased "a[]" form.
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

// Branch merge: moved on either side stays moved; else re-lived wins.
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

// True when every call arg matches a distinct allowed type.
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

        // Each allowed type may be used once.
        for (int j = 0; j < allowedSize; j++)
        {
            if (typesDepleted[j] == true)
            {
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

// True for `arr[i]` reads (casts unwrapped); the array keeps ownership.
static bool IsArrayElementBorrow(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return n && n->kind == NodeIndex;
}

// Array builtins (push/pop/resize); marks the call pseudo. True if handled.
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

    // Unwrap `T[]?` receivers for the element type.
    const TypeName* unwrapped = arrType && arrType->isOptional ? arrType->inner : arrType;

    if (!TypeNameIsDynamicArray(unwrapped))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' expects an array argument, not '%s'", c->callee,
                     arrType ? arrType->name : "");
        return true;
    }

    // The array is mutated, so it must be an lvalue.
    if (arg0->kind != NodeIdent && arg0->kind != NodeMember && arg0->kind != NodeIndex)
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' array argument must be an lvalue", c->callee);
        return true;
    }

    // Length may change: drop `[recv.length]` facts (resize/pop also drop below).
    {
        const char* lenRecvKey = MovableBoxSourceKey(r, arg0);

        if (lenRecvKey)
        {
            InvalidateIndexVar(r, KeyRoot(r->m_arena, lenRecvKey));
        }
    }

    // Resize/pop drop element facts; push keeps them.
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
            // A proven `T?` pushed into `^T[]` moves (source emptied).
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

        // Push moves owning values in; optional sources end up empty.
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

// Bare `...` accepts scalars, strings, and handles (not structs/arrays).
static bool IsCVarargScalarish(Resolver* r, const TypeName* type)
{
    if (!type)
    {
        return false;
    }

    if (IsNumeric(type->name) || TypeIsString(&r->m_registry, type->name))
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

// Bare `...` also accepts `^T` when T is scalar/string/handle.
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

// Move owned args into params; drop facts a real call could break.
static void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c)
{
    // Calls may rebind globals, so global facts never survive.
    for (size_t g = 0; g < r->m_mod->globals.count; g++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->m_mod->globals, g);

        ClearNullableFacts(r, gd->name);
    }

    // A typed rest collects trailing args as its element type.
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = NamedParamCount(best);

    for (size_t j = 0; j < c->args.count; j++)
    {
        Node* arg = (Node*)VecGet(&c->args, j);

        // A non-const `ref` arg may be mutated: drop its facts.
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
                    // The callee may write through the ref: drop nested facts.
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

// `T?` args unwrapped into plain targets must be proven non-empty.
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
                target = TypeNameBoxInner(at);
            }

            CheckOptionalDeref(r, arg, at, target, arg->range);
        }

        return;
    }

    // Struct constructor: field slots are the targets.
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

    // array_push: the element slot is the target.
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

// Rewrite a braced list as a positional struct init of `structName`.
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

// Give a bare braced node the struct target type (no-op otherwise).
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

// -- Impl blocks.

// First impl block for a type, or NULL.
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

static EnumDecl* FindEnum(Resolver* r, const char* name)
{
    for (size_t i = 0; i < r->m_mod->enums.count; i++)
    {
        EnumDecl* ed = (EnumDecl*)VecGet(&r->m_mod->enums, i);

        if (strcmp(ed->name, name) == 0)
        {
            return ed;
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

/* If `n` is a property read on an impl target (after unwrapping `T?`/`^T`),
   returns the property; NULL when the member is not an impl property. */
static PropertyDecl* PropertyOnHandle(Resolver* r, const TypeName* baseType, const char* member)
{
    const TypeName* inner = UnwrapBoxPtr(baseType);

    if (!inner || !TypeRegistryIsImplTarget(&r->m_registry, inner->name))
    {
        return NULL;
    }

    return FindImplProperty(r, inner->name, member);
}

/* Resolves `EnumName.Member` — a scoped enum constant read. Returns true when
   the member expression was handled (marked as an enum constant, or diagnosed
   as a bad member). Only bare type-name bases are considered; a variable
   shadowing the enum name takes precedence. */
static bool TryResolveEnumMember(Resolver* r, MemberExpr* m, StrMap* scope)
{
    if (m->base_node->kind != NodeIdent)
    {
        return false;
    }

    const char* baseName = ((IdentExpr*)m->base_node)->name;

    if (StrMapGet(scope, baseName))
    {
        return false; /* a local shadows the enum type name */
    }

    if (!TypeRegistryIsEnum(&r->m_registry, baseName))
    {
        return false;
    }

    EnumDecl* ed = FindEnum(r, baseName);

    if (!ed)
    {
        return false;
    }

    for (size_t i = 0; i < ed->members.count; i++)
    {
        EnumMemberDecl* member = (EnumMemberDecl*)VecGet(&ed->members, i);

        if (strcmp(member->name, m->member) == 0)
        {
            m->isEnumConst = true;
            m->enumValue = member->value;
            m->enumTypeName = baseName;
            return true;
        }
    }

    DiagErrorFmt(r->m_diag, m->base.range, "enum '%s' has no member '%s'", baseName, m->member);
    return true;
}

/* Resolves `int.max` / `float.min` — a pseudo-property read on a BUILTIN
   scalar type name (aliases and user types are excluded; only the builtin
   spellings carry the C-limit constants). Returns true when the base names
   a scalar pseudo type, meaning the member expression was fully handled:
   a valid `max`/`min` marks the member as a constant; an invalid member is
   diagnosed. Anything else (value bases, non-scalar bases) returns false. */
static bool TryResolveScalarPseudoConst(Resolver* r, MemberExpr* m)
{
    if (m->base_node->kind != NodeIdent)
    {
        return false;
    }

    const char* baseName = ((IdentExpr*)m->base_node)->name;

    if (!IsScalarPseudoType(baseName))
    {
        return false;
    }

    if (!ScalarPseudoConst(baseName, m->member, NULL, NULL, NULL))
    {
        DiagErrorFmt(r->m_diag, m->base.range, "type '%s' has no member '%s'", baseName, m->member);
        return true;
    }

    m->isScalarConst = true;
    return true;
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

        if (!inner || !TypeRegistryIsImplTarget(&r->m_registry, inner->name))
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
            DiagErrorFmt(r->m_diag, c->base.range, "type '%s' has no method '%s'", handleName, c->callee);
        }

        return true;
    }

    /* Instance call: when the method's first parameter is the receiver slot
       (the impl type the receiver's type extends or equals), prepend the base
       expression as the self argument. Parameterless methods (factories) and
       static calls map arguments 1:1. */
    if (!isStatic && method->params.count > 0)
    {
        const ParamDecl* p0 = (const ParamDecl*)VecGet(&method->params, 0);

        if (TypeRegistryIsImplTarget(&r->m_registry, p0->type.name)
            && (strcmp(p0->type.name, handleName) == 0
                || HandleExtendsFrom(&r->m_registry, handleName, p0->type.name)))
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
    size_t matchCount = 0;
    FunctionDecl* match = NULL;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(fn->name, symbol) != 0)
        {
            continue;
        }

        matchCount++;
        match = fn;
    }

    if (matchCount > 1)
    {
        /* An accessor must name a unique function: with overloads there is no
           way to tell which one `get`/`set` refers to, and first-match binding
           would silently depend on declaration order. */
        DiagErrorFmt(diag, range,
                     "property %s '%s' is ambiguous: %zu functions share that name; "
                     "property accessors must name a unique function",
                     isSetter ? "setter" : "getter", symbol, matchCount);
        return;
    }

    if (match)
    {
        /* Validate the pre-existing declaration against the accessor shape:
           getter (self) -> propType; setter (self, value propType) -> void.
           A getter may return the value via a plain return OR a trailing
           `return propType` out-param (`void Get(self, return T c)`) — the
           parser folds the out-param's type into fn->returnType and
           NamedParamCount excludes it, so the checks below cover both forms. */
        size_t wantParams = isSetter ? 2 : 1;

        if (NamedParamCount(match) != wantParams)
        {
            DiagErrorFmt(diag, range,
                         "property %s '%s' must take (%s self%s); found %zu parameter(s)",
                         isSetter ? "setter" : "getter", symbol, handleName, isSetter ? ", value" : "",
                         match->params.count);
            return;
        }

        const ParamDecl* p0 = (const ParamDecl*)VecGet(&match->params, 0);

        if (strcmp(p0->type.name, handleName) != 0)
        {
            DiagErrorFmt(diag, range, "property %s '%s' must take '%s' as its first parameter; found '%s'",
                         isSetter ? "setter" : "getter", symbol, handleName, p0->type.name);
            return;
        }

        if (isSetter)
        {
            if (strcmp(match->returnType.name, "void") != 0)
            {
                DiagErrorFmt(diag, range, "property setter '%s' must return void; found '%s'", symbol,
                             match->returnType.name);
                return;
            }

            const ParamDecl* p1 = (const ParamDecl*)VecGet(&match->params, 1);

            if (strcmp(p1->type.name, propType->name) != 0)
            {
                DiagErrorFmt(diag, range, "property setter '%s' must take '%s' as its value parameter; found '%s'",
                             symbol, propType->name, p1->type.name);
            }
        }
        else if (strcmp(match->returnType.name, propType->name) != 0)
        {
            DiagErrorFmt(diag, range, "property getter '%s' must return '%s'; found '%s'", symbol, propType->name,
                         match->returnType.name);
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
/* Result of folding an enum member value expression. */
typedef enum {
    EnumEvalOk = 0,
    EnumEvalNotConst,      /* not a constant integer expression (incl. div-by-zero, bad shift) */
    EnumEvalNegForUnsigned, /* a unary minus in an expression for an unsigned underlying */
    EnumEvalForwardRef,    /* references a member that isn't resolved yet (self/forward) */
} EnumEvalStatus;

/* Finds `memberName` in enum `ed`; returns true with `*outIndex` set. */
static bool FindEnumMemberIndex(EnumDecl* ed, const char* memberName, size_t* outIndex)
{
    for (size_t i = 0; i < ed->members.count; i++)
    {
        EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, i);

        if (strcmp(m->name, memberName) == 0)
        {
            *outIndex = i;
            return true;
        }
    }

    return false;
}

/* Width of a constant expression: 32-bit if it bottoms out in a 32-bit literal. */
static unsigned ConstBitWidth(const Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((const CastExpr*)n)->operand;
    }

    if (n && n->kind == NodeIntLiteral)
    {
        const IntLiteral* lit = (const IntLiteral*)n;

        if (lit->isUnsigned && lit->value <= 0x7FFFFFFFULL)
        {
            return 32;
        }
    }

    return 64;
}

/* Folds an enum member value expression to a 64-bit value */
static EnumEvalStatus EnumEvalConstExpr(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, bool isUnsigned,
                                        uint64_t* out)
{
    if (!n)
    {
        return EnumEvalNotConst;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        *out = ((IntLiteral*)n)->value;
        return EnumEvalOk;
    case NodeBoolLiteral:
        *out = ((BoolLiteral*)n)->value ? 1 : 0;
        return EnumEvalOk;
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;
        uint64_t inner = 0;
        EnumEvalStatus st = EnumEvalConstExpr(u->operand, mod, enumIndex, memberIndex, isUnsigned, &inner);

        if (st != EnumEvalOk)
        {
            return st;
        }

        switch (u->op)
        {
        case UnPos:
            *out = inner;
            return EnumEvalOk;
        case UnNeg:
            if (isUnsigned)
            {
                /* `-1u` on an unsigned underlying wraps C-style to 0xFFFFFFFF
                   (a plain `-1` stays rejected). */
                if (ConstBitWidth(u->operand) == 32)
                {
                    *out = (0 - inner) & 0xFFFFFFFFULL;
                    return EnumEvalOk;
                }

                return EnumEvalNegForUnsigned;
            }

            *out = (uint64_t)(0 - inner);
            return EnumEvalOk;
        case UnBitNot:
            *out = ConstBitWidth(u->operand) == 32 ? (~inner & 0xFFFFFFFFULL) : ~inner;
            return EnumEvalOk;
        default:
            return EnumEvalNotConst;
        }
    }
    case NodeBinary:
    {
        BinaryExpr* e = (BinaryExpr*)n;
        uint64_t l = 0;
        uint64_t r = 0;
        EnumEvalStatus sl = EnumEvalConstExpr(e->lhs, mod, enumIndex, memberIndex, isUnsigned, &l);

        if (sl != EnumEvalOk)
        {
            return sl;
        }

        EnumEvalStatus sr = EnumEvalConstExpr(e->rhs, mod, enumIndex, memberIndex, isUnsigned, &r);

        if (sr != EnumEvalOk)
        {
            return sr;
        }

        switch (e->op)
        {
        case BinAdd:
            *out = l + r;
            return EnumEvalOk;
        case BinSub:
            *out = l - r;
            return EnumEvalOk;
        case BinMul:
            *out = l * r;
            return EnumEvalOk;
        case BinDiv:
            if (r == 0)
            {
                return EnumEvalNotConst;
            }

            *out = isUnsigned ? (l / r) : (uint64_t)((int64_t)l / (int64_t)r);
            return EnumEvalOk;
        case BinMod:
            if (r == 0)
            {
                return EnumEvalNotConst;
            }

            *out = isUnsigned ? (l % r) : (uint64_t)((int64_t)l % (int64_t)r);
            return EnumEvalOk;
        case BinBitAnd:
            *out = l & r;
            return EnumEvalOk;
        case BinBitOr:
            *out = l | r;
            return EnumEvalOk;
        case BinBitXor:
            *out = l ^ r;
            return EnumEvalOk;
        case BinShl:
            if (r >= 64)
            {
                return EnumEvalNotConst;
            }

            *out = (isUnsigned && ConstBitWidth(e->lhs) == 32) ? ((l << r) & 0xFFFFFFFFULL) : (l << r);
            return EnumEvalOk;
        case BinShr:
            if (r >= 64)
            {
                return EnumEvalNotConst;
            }

            *out = isUnsigned ? (l >> r) : (uint64_t)((int64_t)l >> (int64_t)r);
            return EnumEvalOk;
        default:
            return EnumEvalNotConst; /* comparisons / && / || don't fold to an integer value */
        }
    }
    case NodeCast:
        return EnumEvalConstExpr(((CastExpr*)n)->operand, mod, enumIndex, memberIndex, isUnsigned, out);
    case NodeIdent:
    {
        /* A bare name: a reference to an EARLIER member of this enum. */
        IdentExpr* id = (IdentExpr*)n;
        EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, enumIndex);
        size_t refIndex = 0;

        if (!FindEnumMemberIndex(ed, id->name, &refIndex))
        {
            return EnumEvalNotConst;
        }

        if (refIndex >= memberIndex)
        {
            return EnumEvalForwardRef;
        }

        *out = ((EnumMemberDecl*)VecGet(&ed->members, refIndex))->value;
        return EnumEvalOk;
    }
    case NodeMember:
    {
        /* `EnumName.Member` — a scoped constant from an already-resolved enum. */
        MemberExpr* me = (MemberExpr*)n;

        /* `int.max` — a builtin scalar pseudo-property folds to its value
           (integral only; enums never have float underlyings). */
        if (me->base_node->kind == NodeIdent)
        {
            uint64_t intVal = 0;
            bool isFloat = false;

            if (ScalarPseudoConst(((IdentExpr*)me->base_node)->name, me->member, &intVal, NULL, &isFloat))
            {
                if (isFloat)
                {
                    return EnumEvalNotConst;
                }

                *out = intVal;
                return EnumEvalOk;
            }
        }

        if (me->base_node->kind != NodeIdent)
        {
            return EnumEvalNotConst;
        }

        const char* enumName = ((IdentExpr*)me->base_node)->name;

        for (size_t i = 0; i < mod->enums.count; i++)
        {
            EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, i);

            if (strcmp(ed->name, enumName) != 0)
            {
                continue;
            }

            size_t refIndex = 0;

            if (!FindEnumMemberIndex(ed, me->member, &refIndex))
            {
                return EnumEvalNotConst;
            }

            if (i > enumIndex || (i == enumIndex && refIndex >= memberIndex))
            {
                return EnumEvalForwardRef;
            }

            *out = ((EnumMemberDecl*)VecGet(&ed->members, refIndex))->value;
            return EnumEvalOk;
        }

        return EnumEvalNotConst;
    }
    default:
        return EnumEvalNotConst;
    }
}

/* Reports the error for a member whose value expression failed to fold. */
static void ReportEnumValueError(DiagnosticEngine* diag, EnumMemberDecl* m, EnumEvalStatus st, const char* underlying)
{
    switch (st)
    {
    case EnumEvalNegForUnsigned:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' may not be negative for unsigned underlying type '%s'",
                     m->name, underlying);
        break;
    case EnumEvalForwardRef:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' value forward-references a member that is not resolved yet",
                     m->name);
        break;
    default:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' value must be a constant expression", m->name);
        break;
    }
}

/* Assigns enum member values (sequential from 0, or a folded constant
   expression) and validates every value fits the underlying scalar integral
   type. Emits the enum's underlying type in `registry` (registered as an alias
   earlier). */
static void ResolveEnums(Module* mod, DiagnosticEngine* diag, const TypeRegistry* registry)
{
    for (size_t i = 0; i < mod->enums.count; i++)
    {
        EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, i);

        const char* underlying = ed->underlyingType ? ed->underlyingType : "int";
        const char* resolved = TypeRegistryResolveAlias(registry, underlying);

        if (!IsEnumUnderlyingType(resolved))
        {
            DiagErrorFmt(diag, ed->base.range, "enum '%s' underlying type '%s' must be a scalar integral type",
                         ed->name, underlying);
            continue;
        }

        for (size_t j = 0; j < ed->members.count; j++)
        {
            EnumMemberDecl* a = (EnumMemberDecl*)VecGet(&ed->members, j);

            for (size_t k = j + 1; k < ed->members.count; k++)
            {
                EnumMemberDecl* b = (EnumMemberDecl*)VecGet(&ed->members, k);

                if (strcmp(a->name, b->name) == 0)
                {
                    DiagErrorFmt(diag, b->base.range, "duplicate enum member '%s' in '%s'", b->name, ed->name);
                }
            }
        }

        bool isUnsigned = false;
        int64_t minVal = 0;
        uint64_t maxVal = 0;
        EnumSignedRange(resolved, &isUnsigned, &minVal, &maxVal);

        if (isUnsigned)
        {
            uint64_t next = 0;

            for (size_t j = 0; j < ed->members.count; j++)
            {
                EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, j);

                if (m->hasExplicitValue)
                {
                    uint64_t v = 0;
                    EnumEvalStatus st = EnumEvalConstExpr(m->valueExpr, mod, i, j, true, &v);

                    if (st != EnumEvalOk)
                    {
                        ReportEnumValueError(diag, m, st, underlying);
                        m->hasExplicitValue = false;
                    }
                    else
                    {
                        m->value = v;
                        m->isNegative = false;
                    }
                }

                if (m->hasExplicitValue)
                {
                    if (m->isNegative)
                    {
                        DiagErrorFmt(diag, m->base.range,
                                     "enum member '%s' may not be negative for unsigned underlying type '%s'",
                                     m->name, underlying);
                        continue;
                    }

                    if (m->value > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %llu does not fit in '%s'",
                                     m->name, (unsigned long long)m->value, underlying);
                        continue;
                    }

                    next = m->value + 1;
                }
                else
                {
                    if (next > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %llu does not fit in '%s'",
                                     m->name, (unsigned long long)next, underlying);
                        continue;
                    }

                    m->value = next;
                    next++;
                }
            }
        }
        else
        {
            int64_t next = 0;

            for (size_t j = 0; j < ed->members.count; j++)
            {
                EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, j);
                int64_t sv = next;

                if (m->hasExplicitValue)
                {
                    uint64_t v = 0;
                    EnumEvalStatus st = EnumEvalConstExpr(m->valueExpr, mod, i, j, false, &v);

                    if (st != EnumEvalOk)
                    {
                        ReportEnumValueError(diag, m, st, underlying);
                        m->hasExplicitValue = false;
                    }
                    else
                    {
                        m->isNegative = (int64_t)v < 0;
                        m->value = m->isNegative ? ((v == (uint64_t)0x8000000000000000ULL) ? v : (uint64_t)(-(int64_t)v))
                                                 : v;
                    }
                }

                if (m->hasExplicitValue)
                {
                    if (!EnumSignedValue(m->value, m->isNegative, &sv))
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value does not fit in '%s'", m->name,
                                     underlying);
                        continue;
                    }

                    next = sv;
                }

                if (sv < minVal || sv > (int64_t)maxVal)
                {
                    DiagErrorFmt(diag, m->base.range, "enum member '%s' value %lld does not fit in '%s'", m->name,
                                 (long long)sv, underlying);
                    continue;
                }

                m->value = (uint64_t)sv;
                next = sv + 1;
            }
        }
    }
}

static void ResolveImpls(Module* mod, DiagnosticEngine* diag, Arena* arena, const TypeRegistry* registry)
{
    for (size_t i = 0; i < mod->impls.count; i++)
    {
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, i);

        if (!TypeRegistryIsImplTarget(registry, impl->handleName))
        {
            DiagErrorFmt(diag, impl->base.range, "impl type '%s' is not a declared struct or handle",
                         impl->handleName);
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
        // Struct ctor: owning field from owning source is a move.
        const StructType* st = TypeRegistryFind(&r->m_registry, c->callee);

        for (size_t j = 0; j < c->args.count && st && j < st->fields.count; j++)
        {
            FieldDecl* fd = (FieldDecl*)VecGet(&st->fields, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            // A braced arg takes the field's struct/array shape.
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

            if (resolvedArg != arg)
            {
                VecSet(&c->args, j, resolvedArg);
            }
            else if (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
            {
                // Braced array field: fill the element type and move-mark.
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

        // Check facts before moves clear them.
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
        else if (NamedParamCount(functionDecl) != c->args.count)
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

            // Extra args collect into the rest tail.
            bool isTail
                = functionDecl->isVariadic
                  && j >= (functionDecl->isCVararg ? functionDecl->params.count : functionDecl->params.count - 1);

            if (functionDecl->isCVararg && isTail)
            {
                if (!IsCVarargCompatible(r, argType))
                {
                    viable = false;
                    break;
                }

                // Prefer exact-arity overloads over varargs.
                score += 2;
                continue;
            }

            const ParamDecl* param
                = (ParamDecl*)VecGet(&functionDecl->params, isTail ? functionDecl->params.count - 1 : j);

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
                // `T` and `^T` coerce into `T?` when inners match.
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
            else if (functionDecl->isExtern && TypeNameIsOwning(paramType)
                     && (TypeNameIsDynamicArray(argType) || TypeNameIsFixedArray(argType)))
            {
                /* Array decay at the extern boundary: a T[] / T[N] argument
                   satisfies a ^T param by passing a pointer to its first
                   element (the host sees T*). Checked before the generic
                   owning-arg rule: dynamic arrays are owning too. */
                const TypeName* paramInner = TypeNameBoxInner(paramType);
                const TypeName* argElem = TypeNameArrayElem(argType);

                if (paramInner && argElem && strcmp(paramInner->name, argElem->name) == 0)
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

                // Owned rest tails box `T` into `^T` inline.
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

    // Reported ambiguity: stop before side effects cause follow-on errors.
    if (ambiguous)
    {
        return;
    }

    /* A `return` param's caller allocates the out slot, so the return type
       must be a defined (sized) struct at every call site. The declaration
       itself may use a forward-declared type; only the call requires it. */
    if (FunctionHasReturnParam(best) && IsIncompleteStruct(&r->m_registry, best->returnType.name))
    {
        DiagErrorFmt(r->m_diag, c->base.range, "call to '%s' has incomplete return type '%s'", best->name,
                     best->returnType.name);
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

        /* Scoped enum constant: `EnumName.Member` has the enum's type. */
        if (m->isEnumConst && m->enumTypeName)
        {
            return InternTypeName(r, m->enumTypeName);
        }

        /* Builtin scalar pseudo-property: `int.max` / `float.min` has the
           base scalar's own type. Resolution-order independent (like the
           impl-property path below). */
        if (m->base_node->kind == NodeIdent)
        {
            const char* baseName = ((IdentExpr*)m->base_node)->name;

            if (IsScalarPseudoType(baseName) && ScalarPseudoConst(baseName, m->member, NULL, NULL, NULL))
            {
                return InternTypeName(r, baseName);
            }
        }

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

        // `string.length` is the fat length, like arrays.
        if (strcmp(m->member, "length") == 0 && TypeIsString(&r->m_registry, baseType->name))
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

        if (baseType && TypeIsString(&r->m_registry, baseType->name))
        {
            // `s[i]` is a bounds-checked byte.
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

        // `T?` and `^T` share an ABI; they convert when inners match.
        const TypeName* valueInner = TypeNameBoxInner(valueType);

        if (targetType->isOptional && valueType->isBox && inner && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        return (inner && strcmp(inner->name, valueType->name) == 0) || strcmp(valueType->name, targetType->name) == 0;
    }

    // Exact name match.
    if (strcmp(valueType->name, targetType->name) == 0)
    {
        return true;
    }

    // Numerics convert freely (TODO: require casts for lossy narrowing).
    if (IsNumeric(valueType->name) && IsNumeric(targetType->name))
    {
        return true;
    }

    // Vectors: same lane count assigns; scalars splat-broadcast.
    const char* resolvedTarget = TypeRegistryResolveAlias(&r->m_registry, targetType->name);
    const char* resolvedValue = TypeRegistryResolveAlias(&r->m_registry, valueType->name);
    const int isTargetVector = IsSimdVector(resolvedTarget);
    const int isValueVector = IsSimdVector(resolvedValue);

    if (isTargetVector && isValueVector && isTargetVector == isValueVector)
    {
        // Same-lane vectors need the same spelling (aliases don't mix).
        if (strcmp(targetType->name, valueType->name) == 0)
        {
            return true;
        }
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
            // Whole use rejects moved values; member bases only fully-moved ones.
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

        // Arithmetic needs numerics or two same-shape vectors.
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

            // Unknown types and box/optional operands unwrap before checking.
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
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
        {
            const TypeName* lt = InferType(r, b->lhs, scope);
            const TypeName* rt = InferType(r, b->rhs, scope);
            const char* ln = lt ? lt->name : "";
            const char* rn = rt ? rt->name : "";

            bool lString = lt && TypeIsString(&r->m_registry, ln);
            bool rString = rt && TypeIsString(&r->m_registry, rn);

            if (lString != rString)
            {
                DiagErrorFmt(r->m_diag, b->base.range, "cannot compare 'string' with '%s'", lString ? rn : ln);
                break;
            }

            if (lString)
            {
                break;
            }

            /* Aggregates (structs, dynamic/fixed arrays, `T[]?`): `==`/`!=`
               compare structurally — same resolved type only, never across
               distinct struct/array types or against scalars. Ordering is
               meaningless for aggregates and rejected. */
            if (lt && rt
                && (TypeIsComparableAggregate(&r->m_registry, lt) || TypeIsComparableAggregate(&r->m_registry, rt)))
            {
                bool eqOp = (b->op == BinEqEq || b->op == BinNotEq);

                if (!eqOp || !TypeIsComparableAggregate(&r->m_registry, lt)
                    || !TypeIsComparableAggregate(&r->m_registry, rt) || !SameResolvedType(r, ln, rn))
                {
                    DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln,
                                 rn);
                }

                break;
            }

            if (lt && rt && (!TypeIsTriviallyComparable(&r->m_registry, lt)
                             || !TypeIsTriviallyComparable(&r->m_registry, rt)))
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

            // Writing through an optional needs proven ancestors.
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

        // A bare braced RHS takes its type from the target.
        if ((a->value->kind == NodeArrayInit || a->value->kind == NodeStructInit)
            && (a->target->kind == NodeIdent || a->target->kind == NodeMember))
        {
            const TypeName* at = (a->target->kind == NodeIdent) ? tt : InferType(r, a->target, scope);
            const TypeName* atArr = at && at->isOptional ? at->inner : at;

            // A braced RHS against a struct target is a struct init.
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
            ResolveExpr(r, a->value, scope);
            const TypeName* vt = InferType(r, a->value, scope);

            // `=` rebinds the box; other ops mutate contents. Optionals always rebind.
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

                // A `ref ^T` is the caller's box: assign contents, not the slot.
                if (StrMapGet(&r->m_refBoxParams, targetName))
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "'%s' cannot be reassigned as it is not owned because it is bound as a ref; "
                                 "assign its inner value instead",
                                 targetName);

                    return;
                }

                // Rebinding drops the fact; `=` restores it for provably non-empty values.
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
                        // Moving from `T?` empties the source.
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
                // Contents-assign into ^T (not a move).
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

                // Contents-assign reads through the optional.
                CheckOptionalDeref(r, a->value, vt, inner ? inner : tt, a->base.range);
            }
        }
        else if (targetIsOwningField)
        {
            // Writing an owning field re-lives it.
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
                // Plain `=` restores the field and healed ancestors.
                RevalidateOwningField(r, fieldKey);
            }
            else
            {
                // Compound ops read first, so the field must be live.
                ResolveExpr(r, a->target, scope);
            }

            ResolveExpr(r, a->value, scope);

            if (a->op == AssignSet)
            {
                const TypeName* vt = InferType(r, a->value, scope);
                const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                // Rebind drops stale facts; `=` restores them for proven values.
                bool valueProvesNonEmpty = !vt || !vt->isOptional
                                           || (movedValueKey && IsPathNonEmpty(r, movedValueKey));

                ClearNullableFacts(r, fieldKey);

                if (valueProvesNonEmpty)
                {
                    MarkPathNonEmpty(r, fieldKey);
                }

                // An owning RHS moves out of its source, like box-to-box.
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

            // Enum constants are read-only.
            if (a->target->kind == NodeMember && ((MemberExpr*)a->target)->isEnumConst)
            {
                DiagErrorFmt(r->m_diag, a->base.range, "cannot assign to enum constant '%s.%s'",
                             ((MemberExpr*)a->target)->enumTypeName, ((MemberExpr*)a->target)->member);
                return;
            }

            // Scalar pseudo-properties (`int.max`) are read-only.
            if (a->target->kind == NodeMember && ((MemberExpr*)a->target)->isScalarConst)
            {
                MemberExpr* sm = (MemberExpr*)a->target;
                DiagErrorFmt(r->m_diag, a->base.range, "cannot assign to '%s.%s' (a constant)",
                             ((IdentExpr*)sm->base_node)->name, sm->member);
                return;
            }

            // Whole fixed arrays assign by element, never as a unit.
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

            // `^T` into `T` reads through the box (not a move).
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

        // Scalar pseudo-properties (`int.max`) are read-only.
        if (inc->operand->kind == NodeMember && ((MemberExpr*)inc->operand)->isScalarConst)
        {
            MemberExpr* sm = (MemberExpr*)inc->operand;
            DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s '%s.%s' (a constant)",
                         inc->isDec ? "decrement" : "increment", ((IdentExpr*)sm->base_node)->name, sm->member);
            return;
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

        /* Casting a compile-time constant to an enum: the value must fit the
           enum's underlying range (`(Color)300` for a byte-based enum). */
        if (TypeRegistryIsEnum(&r->m_registry, dstName))
        {
            uint64_t mag = 0;
            bool neg = false;

            if (ResolvedConstIntValue(cast->operand, &mag, &neg) && !EnumConstFits(resolvedDst, mag, neg))
            {
                DiagErrorFmt(r->m_diag, cast->base.range, "enum value does not fit in underlying type '%s'",
                             resolvedDst);
            }
        }

        return;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;

        /* Scoped enum constant: `EnumName.Member` reads a constant — the base
           is a type name, not a value, so skip the normal member path. */
        if (TryResolveEnumMember(r, m, scope))
        {
            return;
        }

        /* Builtin scalar pseudo-property: `int.max` / `float.min`. */
        if (TryResolveScalarPseudoConst(r, m))
        {
            return;
        }

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
            /* Properties and methods work on handles AND on opaque
               (forward-declared) structs; only the "no such member" wording
               differs. */
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
            else if (IsIncompleteStruct(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access a member of incomplete type '%s'",
                             baseType->name);
            }
            else
            {
                DiagErrorFmt(r->m_diag, m->base.range, "handle '%s' has no member '%s'", baseType->name,
                             m->member);
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
                // Nested braced values take the field's struct shape.
                if (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit)
                {
                    field->value = ApplyBracedStructTarget(r, field->value, &fieldDecl->type);
                }

                if (field->value->kind == NodeArrayInit)
                {
                    ArrayInitExpr* ai = (ArrayInitExpr*)field->value;

                    if (fieldDecl->type.isArray && fieldDecl->type.length >= 0)
                    {
                        // Fixed arrays need matching shape; rows lay out row-major.
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

                        // Short rows leave holes that zero-fill.
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

                        // Bare-brace leaves are struct inits of the leaf type.
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
                                continue; // Hole: stays zero.
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
                        // Braced dynamic arrays allocate fresh; fill element type.
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

        // Optional ancestors of a `?` test must already be proven.
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

        // Resolve the operand path itself.
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

            // Nested rows inherit the inner element type.
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

            // Owning elements move out of their sources.
            if (ai->elementType && AliasIsOwning(r, ai->elementType))
            {
                const char* movedKey = MovableBoxSourceKey(r, elem);

                if (movedKey)
                {
                    MoveBoxIdent(r, movedKey, elem->range);
                }
            }

            // Plain elements unwrap `T?` sources.
            CheckOptionalDeref(r, elem, InferType(r, elem, scope), ai->elementType, elem->range);
        }

        return;
    }
    default:
        return;
    }
}

// Loops run twice: once muted to carry state, once for real diagnostics.
static void WalkLoopBody(Resolver* r, Node* body, StrMap* scope, const char* condFactKey, bool condFactNegated)
{
    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    // Muted warmup pass.
    Vec liveLog;
    VecInit(&liveLog);
    r->m_liveLog = &liveLog;

    WalkStmt(r, body, scope);

    r->m_liveLog = NULL;
    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    // Reassigned bindings start the next iteration fresh.
    for (size_t i = 0; i < liveLog.count; i++)
    {
        const char* key = (const char*)VecGet(&liveLog, i);
        ClearBoxSubtree(r, key);
        ClearNullableFacts(r, key);
    }

    // The condition's fact holds through the body.
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

        // Unknown types must fail here so codegen is skipped.
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

        // Owning locals need an init (arrays/optionals exempt).
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

        // Owning structs must live in a box.
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

            // Plain `T` from `T?` unwraps.
            CheckOptionalDeref(r, vd->init, initType, &vd->type, vd->base.range);

            // Box init from a box source moves it.
            const char* movedInitKey = initType && AliasIsOwning(r, &vd->type) && AliasIsOwning(r, initType)
                                           ? MovableBoxSourceKey(r, vd->init)
                                           : NULL;

            // A `T?` init proves non-empty only when itself proven.
            if (vd->type.isOptional)
            {
                initProvesNonEmpty = (initType && !initType->isOptional)
                                     || (movedInitKey && IsPathNonEmpty(r, movedInitKey));
            }

            if (movedInitKey)
            {
                if (initType->isOptional)
                {
                    MoveOptionalSource(r, movedInitKey, vd->base.range);
                }
                else
                {
                    MoveBoxIdent(r, movedInitKey, vd->base.range);
                }
            }
        }

        // Fresh binding: drop stale state from any shadowed same-named var.
        ClearBoxSubtree(r, vd->name);
        ClearNullableFacts(r, vd->name);
        InvalidateIndexVar(r, vd->name);

        // An init proves non-empty unless it is a maybe-empty `T?`.
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
        // Checks run here; the statement executes at block exit.
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

            // Check the returned value against the declared return type.
            if (r->m_currentReturnType && strcmp(r->m_currentReturnType->name, "void") != 0 && typeName
                && !IsAssignableType(r, r->m_currentReturnType, typeName))
            {
                DiagErrorFmt(r->m_diag, rs->base.range,
                             "cannot return a value of type '%s' from a function returning '%s'", typeName->name,
                             r->m_currentReturnType->name);
            }

            // Returning `T?` from `T` unwraps.
            CheckOptionalDeref(r, rs->value, typeName, r->m_currentReturnType, rs->base.range);

            // Moves only when the function returns an owning type.
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

    /* Assign enum member values and validate underlying ranges, BEFORE the
       manifest-constant fold so `const int N = (int)Color.Red;` resolves. */
    ResolveEnums(mod, diag, &r.m_registry);

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

        // Layout errors are computed in the registry; reported here for ranges.
        const StructType* registered = TypeRegistryFind(&r.m_registry, sd->name);

        if (registered && registered->layoutError)
        {
            DiagErrorFmt(diag, sd->base.range, "%s", registered->layoutError);
        }

        for (size_t j = 0; j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

            if (sd->isExtern && (TypeIsString(&r.m_registry, field->type.name) ||
                                 ResolvesToDynamicArray(&r, &field->type)))
            {
                // Strings and dynamic arrays are fats; extern structs must match C layout.
                if (TypeIsString(&r.m_registry, field->type.name))
                {
                    DiagErrorFmt(diag, field->type.range,
                                 "extern struct field '%s' may not have type 'string' "
                                 "(use a '^byte' or integer-typed member for a raw char*)",
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

            // String globals default to empty, like arrays.
            bool stringLike = TypeIsString(&r.m_registry, gd->type.name)
                              || (gd->type.isOptional && gd->type.inner
                                  && TypeIsString(&r.m_registry, gd->type.inner->name));

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
           pointer across the boundary, not a struct. A `return` param (out
           pointer) is exempt: its type comes back through the parameter,
           never in the return register. */
        if (functionDecl->isExtern && !functionDecl->hasReturnParam
            && IsDefinedStruct(&r.m_registry,
                               TypeRegistryResolveAlias(&r.m_registry, functionDecl->returnType.name)))
        {
            DiagError(diag, functionDecl->base.range, "extern function cannot return a struct type by value");
        }

        /* fat-pointer type returns are disallowed for externs */
        if (functionDecl->isExtern && !functionDecl->hasReturnParam)
        {
            const char* leaf = TypeRegistryResolveAlias(&r.m_registry, functionDecl->returnType.name);
            TypeName parsed = TypeNameParse(r.m_arena, leaf);

            const TypeName* unwrapped = parsed.isOptional ? parsed.inner : &parsed;
            bool isStringReturn = TypeIsString(&r.m_registry, parsed.name);
            bool isFat = (unwrapped && TypeNameIsDynamicArray(unwrapped))
                         || (parsed.isOptional && TypeIsString(&r.m_registry, unwrapped ? unwrapped->name : NULL));

            if (isStringReturn)
            {
                DiagErrorFmt(diag, functionDecl->base.range,
                             "extern function cannot return '%s' by value. use return-param (e.g, `return %s paramName`) instead, writing out the return value as a pointer from the host",
                             functionDecl->returnType.name, functionDecl->returnType.name);
            }
            else if (isFat)
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
        if (!functionDecl->hasReturnParam && IsIncompleteStruct(&r.m_registry, functionDecl->returnType.name))
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
