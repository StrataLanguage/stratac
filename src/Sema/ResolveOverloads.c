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
    StrMap m_refLocals;      /* `ref` locals: name -> the (canonical) path they alias */
    StrMap m_refThroughBox;  /* `ref` locals that alias the value inside a box at their path */
    StrMap m_refArrayParams; /* `ref T[]` param names: borrowed binding — no rebind, no push/resize */
    StrMap m_typeCache;      /* canonical spelling -> interned TypeName tree */
    StrMap m_constGlobals;   /* const scalar global name -> ConstGlobalVal* (manifest constants) */
    const TypeName* m_currentReturnType;
    const FunctionDecl* m_currentFunction;
    StrMap* m_curScope; /* the function's scope, for typing move paths (see KeyPathType) */

    /* Vec of Vec*'s, a list of scopes and their respective symbols. */
    Vec m_scopeDecls;

    /* Vec of Vec*'s parallel to m_scopeDecls: each scope's DeferEntry*'s. */
    Vec m_deferFrames;

    /* Vec of LoopFlow*'s: break/continue targets of the enclosing loops. */
    Vec m_loopFlows;

    /* Nesting depth of deferred statements being walked. */
    int m_deferWalkDepth;

    /* The current path is dead (after return/break/continue). */
    bool m_unreachable;

    /* The index expression being resolved is the target of a plain `=`: it
       is overwritten, not read, so a moved-out element is legal there. */
    bool m_indexWriteTarget;
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
    if (!t)
    {
        return false;
    }

    switch (GetPrimitiveType(t))
    {
    case PrimInt:
    case PrimUInt:
    case PrimLong:
    case PrimULong:
    case PrimByte:
    case PrimSByte:
    case PrimShort:
    case PrimUShort:
        return true;
    default:;
    }

    return false;
}

/* Signed bounds for the enum underlying types. */
static void EnumSignedRange(const char* underlying, bool* isUnsigned, int64_t* minVal, uint64_t* maxVal)
{
    switch (GetPrimitiveType(underlying))
    {
    case PrimByte:
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = UCHAR_MAX;
        return;
    case PrimUShort:
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = USHRT_MAX;
        return;
    case PrimUInt:
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = UINT_MAX;
        return;
    case PrimULong:
        *isUnsigned = true;
        *minVal = 0;
        *maxVal = ULLONG_MAX;
        return;
    case PrimSByte:
        *isUnsigned = false;
        *minVal = SCHAR_MIN;
        *maxVal = (uint64_t)SCHAR_MAX;
        return;
    case PrimShort:
        *isUnsigned = false;
        *minVal = SHRT_MIN;
        *maxVal = (uint64_t)SHRT_MAX;
        return;
    case PrimInt:
        *isUnsigned = false;
        *minVal = INT_MIN;
        *maxVal = (uint64_t)INT_MAX;
        return;
    default:;
    }

    /* "long" (and any fallback) */
    *isUnsigned = false;
    *minVal = LLONG_MIN;
    *maxVal = (uint64_t)LLONG_MAX;
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
            && ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)m->base_node)->name), m->member, &intVal, NULL, &isFloat)
            && !isFloat)
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
        *out = (long long)(a + b);
        return true;
    case BinSub:
        *out = (long long)(a - b);
        return true;
    case BinMul:
        *out = (long long)(a * b);
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

        *out = (long long)(a << b);
        return true;
    case BinShr:
        if (b >= 64)
        {
            return false;
        }

        *out = (long long)(a >> b);
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

        if (ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)m->base_node)->name), m->member, &intVal, &floatVal,
                              &isFloat))
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

    if (!leaf)
    {
        return false;
    }

    switch (GetPrimitiveType(leaf))
    {
    case PrimInt:
    case PrimUInt:
    case PrimLong:
    case PrimULong:
    case PrimByte:
    case PrimSByte:
    case PrimShort:
    case PrimUShort:
        return true;
    default:;
    }

    return false;
}

/* A `const` scalar global of any numeric type folds into the manifest table
   (floats/bools inline at uses too — they just cannot size arrays). */
static bool IsManifestConstType(const TypeRegistry* reg, const char* name)
{
    return IsNameNumeric(TypeRegistryResolveAlias(reg, name));
}

static bool IsBoolType(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    return GetPrimitiveType(TypeRegistryResolveAlias(reg, t->name)) == PrimBool;
}

/* Types a boolean context (`if`/`while`/`for`, `!`, `&&`, `||`) accepts:
   `bool`, `^bool` (reads through), and any `T?` (truthiness is presence).
   Numerics are not: `if (x)` on a float is a hidden != 0.0 test. */
static bool IsBoolContextType(const TypeRegistry* reg, const TypeName* t)
{
    if (t && t->isOptional)
    {
        return true;
    }

    return IsBoolType(reg, t && t->isBox && t->inner ? t->inner : t);
}

/* Scalars convert implicitly among themselves, except across the bool
   boundary: `bool b = 0.5` (a != 0 test) or `int n = flag` is almost always
   a bug, so numeric <-> bool needs an explicit cast. */
static bool IsImplicitScalarConversion(PrimitiveType target, PrimitiveType value)
{
    return IsNumeric(target) && IsNumeric(value) && (target == PrimBool) == (value == PrimBool);
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
            DiagErrorFmt(r->m_diag, t->range, "array dimension '%s' must be at least 1 (got %ld)",
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
                t->name = arena_format(r->m_arena, "%.*s[%ld]%s", (int)(open - t->name), t->name, t->length, close + 1);
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

/* True when `member` on `base` is one of the fat pseudo-properties: dynamic
   arrays and strings carry `.length` and `.cap`; fixed arrays expose only a
   compile-time `.length`. `cstring` exposes `.length` only (strlen, no `.cap`).
   All are read-only views of runtime state. */
static bool IsFatPseudoProperty(const Resolver* r, const TypeName* base, const char* member)
{
    bool isData = strcmp(member, "data") == 0;

    if (strcmp(member, "length") != 0 && strcmp(member, "cap") != 0 && !isData)
    {
        return false;
    }

    base = UnwrapBoxPtr(base);

    if (!base)
    {
        return false;
    }

    /* `.data`: the element buffer of a dynamic array or string, as a `ptr` (for FFI). */
    if (isData)
    {
        return TypeNameIsDynamicArray(base) || TypeIsString(&r->m_registry, base->name);
    }

    if (base->isArray && base->length >= 0)
    {
        return strcmp(member, "length") == 0;
    }

    if (TypeIsCString(&r->m_registry, base->name))
    {
        return strcmp(member, "length") == 0;
    }

    return TypeNameIsDynamicArray(base) || TypeIsString(&r->m_registry, base->name);
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
 * single-dimension. Returns false (with a diagnostic) on mismatch. `kind` is "field" or "local" for messages. */
static bool CheckFixedArrayInitShape(Resolver* r, const TypeName* t, ArrayInitExpr* ai, const TypeName* leaf,
                                     const char* name, const char* kind)
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
                         "multidimensional fixed-size array %s '%s' requires nested rows "
                         "('{ { ... }, { ... } }'), not a flat element list",
                         kind, name);
            return false;
        }

        if (!multidim && elem->kind == NodeArrayInit && !leafIsStruct)
        {
            DiagErrorFmt(r->m_diag, elem->range,
                         "fixed-size array %s '%s' has a single dimension - write its elements as a flat list", kind,
                         name);
            return false;
        }

        if (multidim && !CheckFixedArrayInitShape(r, t->elem, (ArrayInitExpr*)elem, leaf, name, kind))
        {
            return false;
        }
    }

    return true;
}

/* True when any element of a fixed-array initializer is itself a braced
   row (`NodeArrayInit`); NULL holes are skipped. */
static bool HasNestedArrayInit(const ArrayInitExpr* ai)
{
    for (size_t k = 0; k < ai->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&ai->elements, k);

        if (elem && elem->kind == NodeArrayInit)
        {
            return true;
        }
    }

    return false;
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

/* Owning VALUE: AliasIsOwning plus owning structs (and their aliases) — a
   `Rec` value owns its fields, so it moves like a box when consumed and
   legalizes `Rec[]` as inline owning storage. */
static bool AliasIsOwningValue(const Resolver* r, const TypeName* t)
{
    return TypeIsOwningValueResolved(&r->m_registry, r->m_arena, t);
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

/* True when `child` is spelled as `parent` or nested under it. Index groups
   compare by spelling only; PathMayAlias handles aliasing indices. */
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

    return next == '.' || next == '[';
}

// The ']' closing the bracket group opened at `open`, or NULL.
static const char* MatchingBracket(const char* open)
{
    int depth = 0;

    for (const char* p = open; *p; p++)
    {
        if (*p == '[')
        {
            depth++;
        }
        else if (*p == ']' && --depth == 0)
        {
            return p;
        }
    }

    return NULL;
}

static bool IndexSpellingIsIntLiteral(const char* s, size_t len)
{
    if (len == 0)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isdigit((unsigned char)s[i]))
        {
            return false;
        }
    }

    return true;
}

/* True when `child` may name the storage of `parent` or something nested
   under it. Index groups alias unless both are distinct integer literals
   (`a[i]` and `a[j]` may be the same element), and the erased "a[]" aliases
   every index. `*isSelf` is set when `child` may be `parent` itself. */
static bool PathMayAlias(const char* parent, const char* child, bool* isSelf)
{
    const char* p = parent;
    const char* c = child;

    while (*p)
    {
        if (*p == '[')
        {
            const char* pe = MatchingBracket(p);
            const char* ce = *c == '[' ? MatchingBracket(c) : NULL;

            if (!pe || !ce)
            {
                return false;
            }

            size_t plen = (size_t)(pe - p - 1);
            size_t clen = (size_t)(ce - c - 1);
            bool sameSpelling = plen == clen && strncmp(p + 1, c + 1, plen) == 0;

            if (!sameSpelling && IndexSpellingIsIntLiteral(p + 1, plen) && IndexSpellingIsIntLiteral(c + 1, clen))
            {
                return false;
            }

            p = pe + 1;
            c = ce + 1;
            continue;
        }

        if (*c != *p)
        {
            return false;
        }

        p++;
        c++;
    }

    if (isSelf)
    {
        *isSelf = *c == '\0';
    }

    return *c == '\0' || *c == '.' || *c == '[';
}

/* Nulls every key in `m` that may alias `key` or a descendant of it (only
   strict descendants when `includeSelf` is false). */
static void ClearAliasedSubtree(StrMap* m, const char* key, bool includeSelf)
{
    if (!key)
    {
        return;
    }

    for (size_t i = 0; i < m->cap; i++)
    {
        bool isSelf = false;

        if (!m->keys[i] || !PathMayAlias(key, m->keys[i], &isSelf))
        {
            continue;
        }

        if (includeSelf || !isSelf)
        {
            m->values[i] = NULL;
        }
    }
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

static void ClearEmptySubtree(Resolver* r, const char* key);
static void ClearNullableFacts(Resolver* r, const char* key);

static void MarkBoxLive(Resolver* r, const char* name)
{
    ClearBoxSubtree(r, name);
    StrMapPut(&r->m_movedBoxes, name, (void*)2);

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

        size_t prefixLen = strlen(prefix);
        bool hasMovedDescendant = false;

        for (size_t i = 0; i < m->cap; i++)
        {
            const char* key = m->keys[i];

            if (!key || m->values[i] == NULL)
            {
                continue;
            }

            if ((m->values[i] == (void*)1 || m->values[i] == (void*)3) && strncmp(key, prefix, prefixLen) == 0
                && key[prefixLen] == '.')
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

// True when an index group of `key` spells the variable `var`.
static bool KeyIndexMentionsVar(const char* key, const char* var)
{
    size_t vlen = strlen(var);

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

            if ((size_t)(p - start) == vlen && strncmp(start, var, vlen) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

// `key` with every non-literal index group erased ("a[i].f" -> "a[].f").
static const char* EraseVariableIndices(Arena* arena, const char* key)
{
    char* out = (char*)arena_alloc(arena, strlen(key) + 1);
    size_t o = 0;

    for (const char* p = key; *p;)
    {
        const char* close = *p == '[' ? MatchingBracket(p) : NULL;

        if (!close)
        {
            out[o++] = *p++;
            continue;
        }

        size_t len = (size_t)(close - p - 1);

        if (IndexSpellingIsIntLiteral(p + 1, len))
        {
            memcpy(out + o, p, len + 2);
            o += len + 2;
        }
        else
        {
            out[o++] = '[';
            out[o++] = ']';
        }

        p = close + 1;
    }

    out[o] = '\0';
    return out;
}

/* A moved element spelled with `var` ("a[i]") becomes an unknown element
   once `var` changes: re-key it erased ("a[]"), which aliases every index,
   so a later write to the new `a[i]` cannot revive it. */
static void EraseMovedIndexVar(Resolver* r, const char* var)
{
    StrMap* m = &r->m_movedBoxes;
    Vec keys;
    Vec states;
    VecInit(&keys);
    VecInit(&states);

    // Collect first: re-keying inserts, which may rehash the map.
    for (size_t i = 0; i < m->cap; i++)
    {
        void* v = m->values[i];

        if (m->keys[i] && (v == (void*)1 || v == (void*)3) && KeyIndexMentionsVar(m->keys[i], var))
        {
            VecPush(&keys, (void*)EraseVariableIndices(r->m_arena, m->keys[i]));
            VecPush(&states, v);
            m->values[i] = NULL;
        }
    }

    for (size_t h = 0; h < keys.count; h++)
    {
        const char* erased = (const char*)VecGet(&keys, h);

        if (StrMapGet(m, erased) != (void*)1)
        {
            StrMapPut(m, erased, VecGet(&states, h));
        }
    }

    free(keys.items);
    free(states.items);
}

// Drop facts indexed by `var` (assigned, inc/dec, or non-const ref).
static void InvalidateIndexVar(Resolver* r, const char* var)
{
    if (!var)
    {
        return;
    }

    EraseMovedIndexVar(r, var);

    Vec* deps = (Vec*)StrMapGet(&r->m_indexDeps, var);

    if (!deps)
    {
        return;
    }

    /* Only the index changed, not the array: drop exactly these spellings. */
    for (size_t i = 0; i < deps->count; i++)
    {
        const char* key = (const char*)VecGet(deps, i);

        ClearNonEmptySubtree(r, key);
        ClearEmptySubtree(r, key);
    }

    /* The deps stay: a branch or loop join may restore a snapshot that still
       holds these facts, and the next mutation must drop them again. */
}

/* Drops `key` and all `key.*` descendants from the non-empty map. */
static void ClearNonEmptySubtree(Resolver* r, const char* key)
{
    ClearSubtreeByPath(&r->m_nonEmptyPaths, key);
}

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

/* The value at `key` changed (written, rebound or moved out): drops both
   nullable narrowing facts for every path that may alias it or sit under it
   (`a[j] = ...` also drops `a[i]` and `a[0]`). */
static void ClearNullableFacts(Resolver* r, const char* key)
{
    ClearAliasedSubtree(&r->m_nonEmptyPaths, key, true);
    ClearAliasedSubtree(&r->m_emptyPaths, key, true);
}

/* The contents under `key` may have changed but `key` itself did not (a
   contents-assign into a box, or a mutable `ref` borrow): drops the facts of
   every path that may sit strictly under it. */
static void ClearNestedNullableFacts(Resolver* r, const char* key)
{
    ClearAliasedSubtree(&r->m_nonEmptyPaths, key, false);
    ClearAliasedSubtree(&r->m_emptyPaths, key, false);
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

    /* A box target takes the optional's value as is (the smart cast): that too needs the proof. */
    if (!targetType || targetType->isOptional)
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

/* The null-test facts `cond` establishes when it evaluates to `whenTrue`: `x?` and `!x?` terms,
   joined by `&&` (each holds when the whole is true) or `||` (each holds when it is false). Keys go
   to `keys`, and whether each says non-empty (1) or empty (2) to `kinds`. */
static void CollectCondFacts(Resolver* r, Node* cond, bool whenTrue, Vec* keys, Vec* kinds)
{
    while (cond && cond->kind == NodeUnary && ((UnaryExpr*)cond)->op == UnNot)
    {
        cond = ((UnaryExpr*)cond)->operand;
        whenTrue = !whenTrue;
    }

    if (!cond)
    {
        return;
    }

    if (cond->kind == NodeNullTest)
    {
        const char* key = MovableBoxSourceKey(r, ((NullTestExpr*)cond)->operand);

        if (key)
        {
            VecPush(keys, (void*)key);
            VecPush(kinds, whenTrue ? (void*)1 : (void*)2);
        }
        return;
    }

    if (cond->kind == NodeBinary)
    {
        BinaryExpr* b = (BinaryExpr*)cond;

        if ((b->op == BinLogicAnd && whenTrue) || (b->op == BinLogicOr && !whenTrue))
        {
            CollectCondFacts(r, b->lhs, whenTrue, keys, kinds);
            CollectCondFacts(r, b->rhs, whenTrue, keys, kinds);
        }
    }
}

static void MarkPathEmpty(Resolver* r, const char* key);
static void CopyStrMap(const StrMap* src, StrMap* dst);
static void ReplaceStrMapContents(StrMap* dst, const StrMap* src);
static void ResolveExpr(Resolver* r, Node* n, StrMap* scope);

/* Applies what `cond` being `whenTrue` proves. Empty facts are applied only when `withEmpty`; with
   `clearEmpty` they instead forget any non-empty fact about the path (a loop exit). */
static void ApplyCondFacts(Resolver* r, Node* cond, bool whenTrue, bool withEmpty, bool clearEmpty)
{
    Vec keys;
    Vec kinds;
    VecInit(&keys);
    VecInit(&kinds);
    CollectCondFacts(r, cond, whenTrue, &keys, &kinds);

    for (size_t i = 0; i < keys.count; i++)
    {
        const char* key = (const char*)VecGet(&keys, i);

        if (VecGet(&kinds, i) == (void*)1)
        {
            MarkPathNonEmpty(r, key);
        }
        else if (clearEmpty)
        {
            ClearNonEmptySubtree(r, key);
        }
        else if (withEmpty)
        {
            MarkPathEmpty(r, key);
        }
    }

    free(keys.items);
    free(kinds.items);
}

/* Resolves `n` with what `cond` being `whenTrue` proves, then forgets it: the right side of
   `&&` / `||` and the arms of `?:`. */
static void ResolveUnderCondition(Resolver* r, Node* n, Node* cond, bool whenTrue, StrMap* scope)
{
    StrMap facts;
    StrMap empty;
    CopyStrMap(&r->m_nonEmptyPaths, &facts);
    CopyStrMap(&r->m_emptyPaths, &empty);

    ApplyCondFacts(r, cond, whenTrue, true, false);
    ResolveExpr(r, n, scope);

    ReplaceStrMapContents(&r->m_nonEmptyPaths, &facts);
    ReplaceStrMapContents(&r->m_emptyPaths, &empty);
    StrMapFree(&facts);
    StrMapFree(&empty);
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
/* The type of a move path ("a.b[i].c" as MovableBoxSourceKey spells it), and whether any step of it reads
   through an `rc<T>` (a shared value, which nothing may be moved out of). NULL when it cannot be typed. */
static const TypeName* KeyPathType(Resolver* r, const char* key, bool* throughRc)
{
    *throughRc = false;

    if (!r->m_curScope || !key)
    {
        return NULL;
    }

    size_t at = 0;

    while (key[at] && key[at] != '.' && key[at] != '[')
    {
        at++;
    }

    const TypeName* t = (const TypeName*)StrMapGet(r->m_curScope, arena_strndup(r->m_arena, key, at));

    while (t && key[at])
    {
        if (t->isRc)
        {
            *throughRc = true;
        }

        if ((t->isBox || t->isOptional) && t->inner)
        {
            t = t->inner;
        }

        if (key[at] == '.')
        {
            size_t start = ++at;

            while (key[at] && key[at] != '.' && key[at] != '[')
            {
                at++;
            }

            const char* field = arena_strndup(r->m_arena, key + start, at - start);
            const StructType* st = TypeRegistryFind(&r->m_registry, TypeRegistryResolveAlias(&r->m_registry, t->name));
            int idx = st ? TypeRegistryFieldIndex(&r->m_registry, st->name, field) : -1;
            t = idx >= 0 ? &((const FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx))->type : NULL;
        }
        else
        {
            int depth = 0;

            do
            {
                depth += key[at] == '[' ? 1 : key[at] == ']' ? -1 : 0;
                at++;
            } while (key[at] && depth > 0);

            t = TypeNameArrayElem(t);
        }
    }

    return t;
}

static void MarkMoved(Resolver* r, const char* name, SourceRange range);

/* Moves the value at `name` out (an owning use). Copying an `rc<T>` retains it instead, which works from
   anywhere: a global, a ref param, a field. Moving anything else out from under an rc is an error. */
static void MoveBoxIdent(Resolver* r, const char* name, SourceRange range)
{
    bool throughRc = false;
    const TypeName* type = KeyPathType(r, name, &throughRc);

    if (type && type->isRc)
    {
        return;
    }

    if (throughRc)
    {
        DiagErrorFmt(r->m_diag, range, "cannot move '%s' out of a shared rc<...> value; copy() it instead", name);
        return;
    }

    MarkMoved(r, name, range);
}

static void InvalidateRefLocals(Resolver* r, const char* key);

/* "'a' is poisoned", naming what was moved out of it and how to avoid that. */
static void DiagPoisoned(Resolver* r, SourceRange range, const char* key)
{
    const StrMap* m = &r->m_movedBoxes;
    size_t kl = strlen(key);
    const char* moved = NULL;

    for (size_t i = 0; i < m->cap && !moved; i++)
    {
        const char* k = m->keys[i];

        if (k && m->values[i] == (void*)1 && strncmp(k, key, kl) == 0 && (k[kl] == '[' || k[kl] == '.'))
        {
            moved = k;
        }
    }

    if (moved && strchr(moved + kl, '['))
    {
        DiagErrorFmt(r->m_diag, range,
                     "'%s' is poisoned: '%s' was moved out of it. To read an element without taking it, bind "
                     "it with 'const ref' (or 'ref') or copy() it; or put a value back in that slot first",
                     key, moved);
    }
    else if (moved)
    {
        DiagErrorFmt(r->m_diag, range, "'%s' is poisoned: '%s' was moved out of it; put a value back first", key,
                     moved);
    }
    else
    {
        DiagErrorFmt(r->m_diag, range, "'%s' is poisoned", key);
    }
}

/* `key` with a `ref` local at its root replaced by the path the local aliases. */
static const char* CanonicalRefKey(Resolver* r, const char* key)
{
    const char* root = KeyRoot(r->m_arena, key);
    const char* aliased = (const char*)StrMapGet(&r->m_refLocals, root);

    return aliased ? arena_format(r->m_arena, "%s%s", aliased, key + strlen(root)) : key;
}

/* Storage at `key` may move, be reallocated or be freed: every `ref` local that reaches its target
   THROUGH `key` dies (is marked moved, and reports "may dangle" at its next use). One that aliases
   `key` itself survives (the slot stays put), unless it aliases the value inside a box there. */
static void InvalidateRefLocals(Resolver* r, const char* key)
{
    if (!key || r->m_refLocals.count == 0)
    {
        return;
    }

    const char* k = CanonicalRefKey(r, key);
    size_t kl = strlen(k);

    for (size_t i = 0; i < r->m_refLocals.cap; i++)
    {
        const char* name = r->m_refLocals.keys[i];
        const char* path = (const char*)r->m_refLocals.values[i];

        if (!name || !path)
        {
            continue;
        }

        bool through = strncmp(path, k, kl) == 0 && (path[kl] == '.' || path[kl] == '[');
        bool intoBox = strcmp(path, k) == 0 && StrMapGet(&r->m_refThroughBox, name);

        if (through || intoBox)
        {
            MarkBoxMoved(r, name);
        }
    }
}

/* `drop(x)` consumes x whatever it is, an rc included. */
static void MarkMoved(Resolver* r, const char* name, SourceRange range)
{
    InvalidateRefLocals(r, name);

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
    ClearNullableFacts(r, name); // Moving out clears the facts (aliasing indices too).
}

/* True when `key`, an ancestor, or an element that may alias it (`a[j]` for
   `a[i]`, the erased `a[]` for any index) was fully moved out. */
static bool IsPathMaybeMoved(const Resolver* r, const char* key)
{
    const StrMap* m = &r->m_movedBoxes;

    for (size_t i = 0; key && i < m->cap; i++)
    {
        if (m->keys[i] && m->values[i] == (void*)1 && PathMayAlias(m->keys[i], key, NULL))
        {
            return true;
        }
    }

    return false;
}

// Moving a `T?` empties the source instead of poisoning it.
static const TypeName* KeyPathType(Resolver* r, const char* key, bool* throughRc);
static void EmptyOptionalSource(Resolver* r, const char* name);

static void MoveOptionalSource(Resolver* r, const char* name, SourceRange range)
{
    (void)range;

    bool throughRc = false;
    const TypeName* type = KeyPathType(r, name, &throughRc);

    /* An `rc<T>?` copied out keeps its value: the copy is another reference. */
    if (type && type->isRc)
    {
        return;
    }

    EmptyOptionalSource(r, name);
}

/* `drop(x)` of an optional: x is now definitely empty, rc or not. */
static void EmptyOptionalSource(Resolver* r, const char* name)
{
    InvalidateRefLocals(r, name);

    ClearNullableFacts(r, name);

    /* The source is now definitely empty. later `path?` tests resolve to
       "definitely empty" rather than an unproven read. */
    MarkPathEmpty(r, name);
}

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

/* A snapshot of the per-path flow state. `m_live` is false for a path that
   cannot be reached (no state yet, or every contributor was dead). */
typedef struct
{
    StrMap m_moved;
    StrMap m_nonEmpty;
    StrMap m_empty;
    bool m_live;
} FlowState;

static void FlowInit(FlowState* s)
{
    StrMapInit(&s->m_moved);
    StrMapInit(&s->m_nonEmpty);
    StrMapInit(&s->m_empty);
    s->m_live = false;
}

static void FlowFree(FlowState* s)
{
    StrMapFree(&s->m_moved);
    StrMapFree(&s->m_nonEmpty);
    StrMapFree(&s->m_empty);
    s->m_live = false;
}

/* Snapshots the current state (dead paths are captured as not live). */
static void FlowCapture(const Resolver* r, FlowState* s)
{
    ReplaceStrMapContents(&s->m_moved, &r->m_movedBoxes);
    ReplaceStrMapContents(&s->m_nonEmpty, &r->m_nonEmptyPaths);
    ReplaceStrMapContents(&s->m_empty, &r->m_emptyPaths);
    s->m_live = !r->m_unreachable;
}

/* Makes `s` the current state. A dead `s` only marks the path dead: the
   maps keep their last state so dead code still resolves against something. */
static void FlowApply(Resolver* r, const FlowState* s)
{
    if (!s->m_live)
    {
        r->m_unreachable = true;
        return;
    }

    ReplaceStrMapContents(&r->m_movedBoxes, &s->m_moved);
    ReplaceStrMapContents(&r->m_nonEmptyPaths, &s->m_nonEmpty);
    ReplaceStrMapContents(&r->m_emptyPaths, &s->m_empty);
    r->m_unreachable = false;
}

// Control-flow join: moved on any path stays moved, facts must hold on all paths.
static void FlowJoinMaps(FlowState* dst, const StrMap* moved, const StrMap* nonEmpty, const StrMap* empty)
{
    if (!dst->m_live)
    {
        ReplaceStrMapContents(&dst->m_moved, moved);
        ReplaceStrMapContents(&dst->m_nonEmpty, nonEmpty);
        ReplaceStrMapContents(&dst->m_empty, empty);
        dst->m_live = true;
        return;
    }

    MergeMovedBoxes(&dst->m_moved, moved);
    MergeNonEmptyFacts(&dst->m_nonEmpty, nonEmpty);
    MergeNonEmptyFacts(&dst->m_empty, empty);
}

static void FlowJoin(FlowState* dst, const FlowState* src)
{
    if (src->m_live)
    {
        FlowJoinMaps(dst, &src->m_moved, &src->m_nonEmpty, &src->m_empty);
    }
}

// Joins the current path into `dst` (a dead path contributes nothing).
static void FlowJoinCurrent(const Resolver* r, FlowState* dst)
{
    if (!r->m_unreachable)
    {
        FlowJoinMaps(dst, &r->m_movedBoxes, &r->m_nonEmptyPaths, &r->m_emptyPaths);
    }
}

// Same non-NULL entries in both maps.
static bool StrMapSameContents(const StrMap* a, const StrMap* b)
{
    for (size_t i = 0; i < a->cap; i++)
    {
        if (a->keys[i] && a->values[i] && StrMapGet(b, a->keys[i]) != a->values[i])
        {
            return false;
        }
    }

    for (size_t i = 0; i < b->cap; i++)
    {
        if (b->keys[i] && b->values[i] && StrMapGet(a, b->keys[i]) != b->values[i])
        {
            return false;
        }
    }

    return true;
}

static bool FlowSame(const FlowState* a, const FlowState* b)
{
    if (a->m_live != b->m_live)
    {
        return false;
    }

    return !a->m_live
           || (StrMapSameContents(&a->m_moved, &b->m_moved) && StrMapSameContents(&a->m_nonEmpty, &b->m_nonEmpty)
               && StrMapSameContents(&a->m_empty, &b->m_empty));
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
static void WalkLoop(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope);
static void ScopePush(Resolver* r);
static void ScopePop(Resolver* r, StrMap* scope);
static bool ExprIsConstantTrue(const Node* n);
static void CheckCallArgOptionalDerefs(Resolver* r, CallExpr* c, StrMap* scope);

static bool IsLengthPseudoMemberBase(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (t->isArray)
    {
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(reg, t->name);
    return TypeIsString(reg, leaf) || TypeIsCString(reg, leaf);
}

/* The name of the `const` binding (global, param or local) that the lvalue
   `target` is rooted at, or NULL when it is mutable. */
static const char* ConstRootName(const Resolver* r, Node* target, StrMap* scope)
{
    Node* base = target;

    while (base && (base->kind == NodeMember || base->kind == NodeIndex || base->kind == NodeCast))
    {
        if (base->kind == NodeMember)
        {
            base = ((MemberExpr*)base)->base_node;
        }
        else if (base->kind == NodeIndex)
        {
            base = ((IndexExpr*)base)->base_node;
        }
        else
        {
            base = ((CastExpr*)base)->operand;
        }
    }

    if (!base || base->kind != NodeIdent)
    {
        return NULL;
    }

    const char* name = ((IdentExpr*)base)->name;
    const TypeName* declared = (const TypeName*)StrMapGet(scope, name);

    // Const locals are declared in `scope` (shadowing-aware); globals and params are in m_constVars.
    if ((declared && declared->isConst) || StrMapGet(&r->m_constVars, name))
    {
        return name;
    }

    return NULL;
}

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range, StrMap* scope)
{
    if (!target)
    {
        return;
    }

    /* String and array's `.length` is readonly */
    if (target->kind == NodeMember)
    {
        MemberExpr* tm = (MemberExpr*)target;

        if (strcmp(tm->member, "length") == 0)
        {
            const TypeName* bt = InferType(r, tm->base_node, scope);

            if (bt && IsLengthPseudoMemberBase(&r->m_registry, bt))
            {
                DiagErrorFmt(r->m_diag, range,
                             "cannot assign to '%s' (a read-only pseudo-member; use 'array_resize' instead)",
                             tm->member);
                return;
            }
        }
    }

    const char* name = ConstRootName(r, target, scope);

    if (name)
    {
        DiagErrorFmt(r->m_diag, range, "'%s' is immutable", name);
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
static bool ResolveVectorConstruct(Resolver* r, CallExpr* c, StrMap* scope)
{
    // The function name is the same as the type name, so we can use TypeUtil functions on it.
    int numLanes = IsNameSimdVector(c->callee);
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

    c->isIntrinsicCall = true;

    return true;
}

/* Private functions are visible only in the file that defines them */
static bool FunctionVisibleTo(const FunctionDecl* fd, uint16_t callerFile)
{
    return !fd->isPrivate || fd->base.range.fileId == callerFile;
}

/* Returns true if the module declares any user-defined function with `name`
   that is visible to `callerFile`.
   `dot`/`cross` are only intrinsic fallbacks when no user overload exists, so a
   user's `dot(Vec3, Vec3)` is never hijacked by the SIMD resolver. */
static bool ModuleHasFunctionNamed(Resolver* r, const char* name, uint16_t callerFile)
{
    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* fd = (FunctionDecl*)VecGet(&r->m_mod->functions, i);
        if (strcmp(fd->name, name) == 0 && FunctionVisibleTo(fd, callerFile))
        {
            return true;
        }
    }
    return false;
}

//-- SIMD vector intrinsics.

static bool ResolveIntrinsicCall(Resolver* r, CallExpr* c, StrMap* scope);

/* Validates vector intrinsic calls (e.g. `dot` and `cross`) */
static bool ResolveVectorIntrinsics(Resolver* r, CallExpr* c, StrMap* scope)
{
    bool isDot = strcmp(c->callee, "dot") == 0;
    bool isCross = strcmp(c->callee, "cross") == 0;

    if (!isDot && !isCross)
    {
        return false;
    }

    /* A user-defined overload takes precedence over the SIMD intrinsic. */
    if (ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
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
    int lanes0 = GetNameSimdVectorLanes(resolved0);
    int lanes1 = GetNameSimdVectorLanes(resolved1);

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

    c->isIntrinsicCall = true;

    return true;
}

static bool ResolveVector2Arg(Resolver* r, CallExpr* c, StrMap* scope)
{
    // User defined functions take precedence
    if (ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
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

    const char* resolved0 = t0 ? TypeRegistryResolveAlias(&r->m_registry, t0->name) : "";
    const char* resolved1 = t1 ? TypeRegistryResolveAlias(&r->m_registry, t1->name) : "";

    int lanes0 = IsNameSimdVector(resolved0);
    int lanes1 = IsNameSimdVector(resolved1);

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

    return true;
}

static bool ResolveVector1Arg(Resolver* r, CallExpr* c, StrMap* scope)
{
    // User defined functions take precedence
    if (ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects 1 argument, got %zu", c->callee, c->args.count);
        return true;
    }

    Node* vA = (Node*)VecGet(&c->args, 0);
    const TypeName* vAType = InferType(r, vA, scope);

    const char* resolved0 = vAType ? TypeRegistryResolveAlias(&r->m_registry, vAType->name) : "";

    int lanes = IsNameSimdVector(resolved0);

    if (lanes == 0)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' expects SIMD vector arguments (float2/float3/float4), not '%s'",
                     c->callee, vAType ? vAType->name : "?");

        return true;
    }

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

    if (!argType || !AliasIsOwningValue(r, argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'copy' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    if (TypeNameHasDrop(&r->m_registry, argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range,
                     "cannot copy '%s': it holds a value with a Drop method, which would then be dropped twice",
                     argType->name);
        return true;
    }

    c->isIntrinsicCall = true;
    return true;
}

/* Result type of `substring(s, start, len)` — always string, or NULL if not
   a substring call. An optional `string?` source unwraps here; the resolve
   half still demands the narrowing fact via CheckOptionalDeref. */
static const TypeName* SubstringBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "substring") != 0)
    {
        return NULL;
    }
    if (c->args.count != 3)
    {
        return NULL;
    }
    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);

    if (argType && argType->isOptional)
    {
        argType = argType->inner;
    }

    if (!argType || !TypeIsString(&r->m_registry, argType->name))
    {
        return NULL;
    }
    return InternTypeName(r, "string");
}

static bool SubstringIndexTypeOk(Resolver* r, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    const char* resolved = TypeRegistryResolveAlias(&r->m_registry, t->name);

    return IsNameNumeric(resolved) && !IsNameFloatType(resolved) && !IsBoolType(&r->m_registry, t);
}

static bool ResolveSubstringBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "substring") != 0)
    {
        return false;
    }

    if (c->args.count != 3)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'substring' expects 3 arguments (string, start, len), got %zu",
                     c->args.count);
        return true;
    }

    Node* arg0 = (Node*)VecGet(&c->args, 0);
    const TypeName* argType = InferType(r, arg0, scope);
    const TypeName* unwrapped = argType && argType->isOptional ? argType->inner : argType;

    if (!unwrapped || !TypeIsString(&r->m_registry, unwrapped->name))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'substring' expects a string argument, not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    /* Reading through a `string?` needs the narrowing fact, like any
       other optional read. */
    CheckOptionalDeref(r, arg0, argType, InternTypeName(r, "string"), arg0->range);

    const char* indexNames[2] = {"start", "len"};

    for (int i = 1; i <= 2; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, (size_t)i);
        const TypeName* indexType = InferType(r, arg, scope);

        if (!SubstringIndexTypeOk(r, indexType))
        {
            DiagErrorFmt(r->m_diag, arg->range, "'substring' %s must be an integer, not '%s'", indexNames[i - 1],
                         indexType ? indexType->name : "");
            return true;
        }
    }

    c->isIntrinsicCall = true;
    return true;
}

/* Result type of `drop(arg)` - always void (NULL TypeName). */
static const TypeName* DropBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    (void)r;
    (void)c;
    (void)scope;
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

    if (!argType || !AliasIsOwningValue(r, argType))
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'drop' expects an owning type (string, ^T, T[]) — not '%s'",
                     argType ? argType->name : "");
        return true;
    }

    // Dropping frees the value: a `const` binding must stay intact.
    const char* constName = ConstRootName(r, arg0, scope);

    if (constName)
    {
        DiagErrorFmt(r->m_diag, arg0->range, "cannot drop '%s'; it is immutable", constName);
        return true;
    }

    c->isIntrinsicCall = true;

    /* Move the source — the box is invalidated after drop(). An optional
       source is left definitely EMPTY (legal to test later), never poisoned,
       like any other move out of a T?. */
    const char* movedKey = MovableBoxSourceKey(r, arg0);
    if (movedKey)
    {
        if (argType->isOptional)
        {
            EmptyOptionalSource(r, movedKey);
        }
        else
        {
            MarkMoved(r, movedKey, arg0->range);
        }
    }

    return true;
}

/* `sizeof<T>()`, `alignof<T>()`, `load<T>(p)`, `store<T>(p, v)`: intrinsics that take a type argument. */
static bool IsTypedMemoryIntrinsic(Resolver* r, const CallExpr* c)
{
    static const char* const names[] = {"sizeof", "alignof", "load", "store"};

    for (size_t i = 0; i < ARRAY_COUNT(names); i++)
    {
        if (strcmp(c->callee, names[i]) == 0)
        {
            return !ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId);
        }
    }

    return false;
}

/* `a + b` where both sides are a string, cstring or string literal: a new owned string. */
static bool IsStringConcat(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt)
{
    if (op != BinAdd || !lt || !rt || lt->isOptional || rt->isOptional)
    {
        return false;
    }

    bool lStr = TypeIsString(&r->m_registry, lt->name) || TypeIsCString(&r->m_registry, lt->name);
    bool rStr = TypeIsString(&r->m_registry, rt->name) || TypeIsCString(&r->m_registry, rt->name);

    return lStr && rStr;
}

/* `a & b`, `a | b`, `a ^ b` on two values of one enum type: flag sets keep their enum type. */
static bool IsEnumFlagsOp(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt)
{
    if (op != BinBitAnd && op != BinBitOr && op != BinBitXor)
    {
        return false;
    }

    return lt && rt && strcmp(lt->name, rt->name) == 0 && TypeRegistryIsEnum(&r->m_registry, lt->name);
}

//-- Growable `ref T[]` parameters.
//
// A `ref T[]` argument may be a stack view: a fixed-size array, an array literal or a varargs
// rest. Growing or rebinding such a binding would realloc/free stack memory, so a parameter the
// body grows is marked `growsArray`, and call sites must hand it a real (heap) dynamic array.

static ParamDecl* RefArrayParamNamed(const FunctionDecl* f, const char* name)
{
    for (size_t i = 0; f && name && i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet((Vec*)&f->params, i);

        if (p->mod == ModRef && !p->isVarargRest && TypeNameIsDynamicArray(&p->type) && strcmp(p->name, name) == 0)
        {
            return p;
        }
    }

    return NULL;
}

/* Does any function callable as `callee` (an impl method when `isMethod`) grow its parameter `index`? */
static bool CalleeGrowsParam(const Module* mod, const char* callee, bool isMethod, size_t index)
{
    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* g = (const FunctionDecl*)VecGet((Vec*)&mod->functions, i);
        const char* name = isMethod ? g->methodName : g->name;

        if (!name || strcmp(name, callee) != 0 || index >= g->params.count)
        {
            continue;
        }

        if (((const ParamDecl*)VecGet((Vec*)&g->params, index))->growsArray)
        {
            return true;
        }
    }

    return false;
}

static void MarkGrowth(FunctionDecl* f, Node* target, bool* changed)
{
    if (target && target->kind == NodeIdent)
    {
        ParamDecl* p = RefArrayParamNamed(f, ((IdentExpr*)target)->name);

        if (p && !p->growsArray)
        {
            p->growsArray = true;
            *changed = true;
        }
    }
}

static void ScanGrowth(const Module* mod, FunctionDecl* f, Node* n, bool* changed)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        for (size_t i = 0; i < ((Block*)n)->statements.count; i++)
        {
            ScanGrowth(mod, f, (Node*)VecGet(&((Block*)n)->statements, i), changed);
        }
        return;
    case NodeReturn:
        ScanGrowth(mod, f, ((ReturnStmt*)n)->value, changed);
        return;
    case NodeIf:
        ScanGrowth(mod, f, ((IfStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((IfStmt*)n)->thenBranch, changed);
        ScanGrowth(mod, f, ((IfStmt*)n)->elseBranch, changed);
        return;
    case NodeWhile:
        ScanGrowth(mod, f, ((WhileStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((WhileStmt*)n)->body, changed);
        return;
    case NodeFor:
        ScanGrowth(mod, f, ((ForStmt*)n)->init, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->update, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->body, changed);
        return;
    case NodeVarDecl:
        ScanGrowth(mod, f, ((VarDeclStmt*)n)->init, changed);
        return;
    case NodeExprStmt:
        ScanGrowth(mod, f, ((ExprStmt*)n)->expr, changed);
        return;
    case NodeDefer:
        ScanGrowth(mod, f, ((DeferStmt*)n)->stmt, changed);
        return;
    case NodeUnary:
        ScanGrowth(mod, f, ((UnaryExpr*)n)->operand, changed);
        return;
    case NodeBinary:
        ScanGrowth(mod, f, ((BinaryExpr*)n)->lhs, changed);
        ScanGrowth(mod, f, ((BinaryExpr*)n)->rhs, changed);
        return;
    case NodeAssign:
        MarkGrowth(f, ((AssignExpr*)n)->target, changed);
        ScanGrowth(mod, f, ((AssignExpr*)n)->value, changed);
        return;
    case NodeIncDec:
        ScanGrowth(mod, f, ((IncDecExpr*)n)->operand, changed);
        return;
    case NodeCast:
        ScanGrowth(mod, f, ((CastExpr*)n)->operand, changed);
        return;
    case NodeMember:
        ScanGrowth(mod, f, ((MemberExpr*)n)->base_node, changed);
        return;
    case NodeIndex:
        ScanGrowth(mod, f, ((IndexExpr*)n)->base_node, changed);
        ScanGrowth(mod, f, ((IndexExpr*)n)->index, changed);
        return;
    case NodeNullTest:
        ScanGrowth(mod, f, ((NullTestExpr*)n)->operand, changed);
        return;
    case NodeConditional:
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->condition, changed);
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->thenExpr, changed);
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->elseExpr, changed);
        return;
    case NodeArrayInit:
        for (size_t i = 0; i < ((ArrayInitExpr*)n)->elements.count; i++)
        {
            ScanGrowth(mod, f, (Node*)VecGet(&((ArrayInitExpr*)n)->elements, i), changed);
        }
        ScanGrowth(mod, f, ((ArrayInitExpr*)n)->repeatCount, changed);
        return;
    case NodeStructInit:
        for (size_t i = 0; i < ((StructInitExpr*)n)->fields.count; i++)
        {
            ScanGrowth(mod, f, ((StructInitField*)VecGet(&((StructInitExpr*)n)->fields, i))->value, changed);
        }
        return;
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;
        bool isMethod = c->calleeBase != NULL;

        ScanGrowth(mod, f, c->calleeBase, changed);

        if (c->callee && c->args.count > 0
            && (strcmp(c->callee, "array_push") == 0 || strcmp(c->callee, "array_resize") == 0
                || strcmp(c->callee, "array_pop") == 0 || strcmp(c->callee, "array_remove") == 0
                || strcmp(c->callee, "array_swap_remove") == 0 || strcmp(c->callee, "array_reserve") == 0))
        {
            MarkGrowth(f, (Node*)VecGet(&c->args, 0), changed);
        }

        /* A receiver passed as a method's `self`. */
        if (isMethod && c->callee && CalleeGrowsParam(mod, c->callee, true, 0))
        {
            MarkGrowth(f, c->calleeBase, changed);
        }

        for (size_t i = 0; i < c->args.count; i++)
        {
            Node* arg = (Node*)VecGet(&c->args, i);

            if (c->callee && CalleeGrowsParam(mod, c->callee, isMethod, isMethod ? i + 1 : i))
            {
                MarkGrowth(f, arg, changed);
            }

            ScanGrowth(mod, f, arg, changed);
        }
        return;
    }
    default:
        return;
    }
}


//-- Read-only by-value parameters borrow.
//
// A by-value `string`, `T[]` or owning-struct parameter would take its argument (move it out of
// the caller). When the body never consumes it - never moves it or a part of it anywhere, never
// writes to it, never passes it on to something that does - it is passed as `const ref` instead,
// so callers keep their value. Decided before resolution, to a fixpoint over calls; a miss shows
// up as a const error in the body, never as a wrong program.

static bool BorrowCandidateType(Resolver* r, const TypeName* t)
{
    if (t->isBox || t->isOptional || t->isRc || TypeNameIsFixedArray(t))
    {
        return false;
    }

    const char* name = TypeRegistryResolveAlias(&r->m_registry, t->name);

    return TypeIsString(&r->m_registry, t->name) || TypeNameIsDynamicArray(t)
           || (name && TypeRegistryIsOwningStruct(&r->m_registry, name) && !TypeNameHasDrop(&r->m_registry, t));
}

/* The candidate parameter at the root of a path (`p`, `p.a[i].b`), or NULL. */
static ParamDecl* BorrowRoot(FunctionDecl* f, Node* n)
{
    while (n && (n->kind == NodeMember || n->kind == NodeIndex || n->kind == NodeCast))
    {
        n = n->kind == NodeMember  ? ((MemberExpr*)n)->base_node
            : n->kind == NodeIndex ? ((IndexExpr*)n)->base_node
                                   : ((CastExpr*)n)->operand;
    }

    if (!n || n->kind != NodeIdent)
    {
        return NULL;
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);

        if (p->borrowCandidate && strcmp(p->name, ((IdentExpr*)n)->name) == 0)
        {
            return p;
        }
    }

    return NULL;
}

/* The declared type at the end of a path rooted at a parameter; NULL when it is not an owning value
   (a scalar, `.length`, ...). Unknown shapes count as owning. */
static const TypeName* BorrowPathType(Resolver* r, ParamDecl* root, Node* n)
{
    if (n->kind == NodeIdent)
    {
        return &root->type;
    }

    if (n->kind == NodeCast)
    {
        return &((CastExpr*)n)->type; /* `(Alias)p` retags; `(cstring)p` only views */
    }

    const TypeName* t = NULL;

    if (n->kind == NodeIndex)
    {
        t = BorrowPathType(r, root, ((IndexExpr*)n)->base_node);
        return t ? (TypeNameArrayElem(t) ? TypeNameArrayElem(t) : NULL) : NULL;
    }

    if (n->kind != NodeMember)
    {
        return &root->type;
    }

    t = BorrowPathType(r, root, ((MemberExpr*)n)->base_node);

    if (!t)
    {
        return NULL;
    }

    if (t->isBox || t->isOptional)
    {
        t = TypeNameBoxInner(t);
    }

    const char* member = ((MemberExpr*)n)->member;
    const char* name = t ? TypeRegistryResolveAlias(&r->m_registry, t->name) : NULL;
    const StructType* st = name ? TypeRegistryFind(&r->m_registry, name) : NULL;

    if (!st)
    {
        return NULL; /* `.length`, `.data`, swizzles: not owning */
    }

    for (size_t i = 0; i < st->fields.count; i++)
    {
        const FieldDecl* fd = (const FieldDecl*)VecGet((Vec*)&st->fields, i);

        if (strcmp(fd->name, member) == 0)
        {
            return &fd->type;
        }
    }

    return &root->type;
}

static void ConsumeParam(ParamDecl* p, bool* changed)
{
    if (p && p->borrowCandidate)
    {
        p->borrowCandidate = false;
        *changed = true;
    }
}

/* `n` is used where an owning value is taken (moved): consumes its root if it is an owning path. */
static void ConsumeIfTaken(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ParamDecl* p = BorrowRoot(f, n);

    if (p)
    {
        const TypeName* t = BorrowPathType(r, p, n);

        if (t && AliasIsOwningValue(r, t))
        {
            ConsumeParam(p, changed);
        }
    }
}

static bool IsArrayMutator(const char* callee)
{
    return strcmp(callee, "array_push") == 0 || strcmp(callee, "array_resize") == 0
           || strcmp(callee, "array_pop") == 0 || strcmp(callee, "array_remove") == 0
           || strcmp(callee, "array_swap_remove") == 0 || strcmp(callee, "array_reserve") == 0;
}

static void ScanBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed);

/* An argument passed to parameter `index` of whatever `callee` names. */
static void ScanBorrowArg(Resolver* r, FunctionDecl* f, CallExpr* c, Node* arg, size_t index, bool isMethod,
                          bool* changed)
{
    ParamDecl* root = BorrowRoot(f, arg);

    if (!root)
    {
        return;
    }

    bool found = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        const FunctionDecl* g = (const FunctionDecl*)VecGet(&r->m_mod->functions, i);
        const char* name = isMethod ? g->methodName : g->name;

        if (!name || strcmp(name, c->callee) != 0)
        {
            continue;
        }

        found = true;

        if (index >= g->params.count)
        {
            if (!g->isCVararg)
            {
                ConsumeIfTaken(r, f, arg, changed); /* a typed rest takes its elements */
            }
            continue;
        }

        const ParamDecl* q = (const ParamDecl*)VecGet((Vec*)&g->params, index);

        if (q->mod == ModRef && !q->type.isConst)
        {
            ConsumeParam(root, changed); /* written through */
        }
        else if (q->mod == ModNone && !g->isExtern && !q->borrowCandidate && !q->inferredBorrow)
        {
            ConsumeIfTaken(r, f, arg, changed); /* taken */
        }
    }

    if (!found)
    {
        ConsumeParam(root, changed); /* a delegate, or something unknown */
    }
}

static void ScanBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        for (size_t i = 0; i < ((Block*)n)->statements.count; i++)
        {
            ScanBorrows(r, f, (Node*)VecGet(&((Block*)n)->statements, i), changed);
        }
        return;
    case NodeReturn:
        ConsumeIfTaken(r, f, ((ReturnStmt*)n)->value, changed);
        ScanBorrows(r, f, ((ReturnStmt*)n)->value, changed);
        return;
    case NodeIf:
        ScanBorrows(r, f, ((IfStmt*)n)->condition, changed);
        ScanBorrows(r, f, ((IfStmt*)n)->thenBranch, changed);
        ScanBorrows(r, f, ((IfStmt*)n)->elseBranch, changed);
        return;
    case NodeWhile:
        ScanBorrows(r, f, ((WhileStmt*)n)->condition, changed);
        ScanBorrows(r, f, ((WhileStmt*)n)->body, changed);
        return;
    case NodeFor:
        ScanBorrows(r, f, ((ForStmt*)n)->init, changed);
        ScanBorrows(r, f, ((ForStmt*)n)->condition, changed);
        ScanBorrows(r, f, ((ForStmt*)n)->update, changed);
        ScanBorrows(r, f, ((ForStmt*)n)->body, changed);
        return;
    case NodeVarDecl:
    {
        VarDeclStmt* vd = (VarDeclStmt*)n;

        if (vd->isRef)
        {
            if (!vd->type.isConst)
            {
                ConsumeParam(BorrowRoot(f, vd->init), changed); /* a mutable alias */
            }
        }
        else
        {
            ConsumeIfTaken(r, f, vd->init, changed);
        }
        ScanBorrows(r, f, vd->init, changed);
        return;
    }
    case NodeExprStmt:
        ScanBorrows(r, f, ((ExprStmt*)n)->expr, changed);
        return;
    case NodeDefer:
        ScanBorrows(r, f, ((DeferStmt*)n)->stmt, changed);
        return;
    case NodeUnary:
        ScanBorrows(r, f, ((UnaryExpr*)n)->operand, changed);
        return;
    case NodeBinary:
        ScanBorrows(r, f, ((BinaryExpr*)n)->lhs, changed);
        ScanBorrows(r, f, ((BinaryExpr*)n)->rhs, changed);
        return;
    case NodeAssign:
        ConsumeParam(BorrowRoot(f, ((AssignExpr*)n)->target), changed);
        ConsumeIfTaken(r, f, ((AssignExpr*)n)->value, changed);
        ScanBorrows(r, f, ((AssignExpr*)n)->target, changed);
        ScanBorrows(r, f, ((AssignExpr*)n)->value, changed);
        return;
    case NodeIncDec:
        ConsumeParam(BorrowRoot(f, ((IncDecExpr*)n)->operand), changed);
        ScanBorrows(r, f, ((IncDecExpr*)n)->operand, changed);
        return;
    case NodeCast:
        ScanBorrows(r, f, ((CastExpr*)n)->operand, changed);
        return;
    case NodeMember:
        ScanBorrows(r, f, ((MemberExpr*)n)->base_node, changed);
        return;
    case NodeIndex:
        ScanBorrows(r, f, ((IndexExpr*)n)->base_node, changed);
        ScanBorrows(r, f, ((IndexExpr*)n)->index, changed);
        return;
    case NodeNullTest:
        ScanBorrows(r, f, ((NullTestExpr*)n)->operand, changed);
        return;
    case NodeConditional:
        ScanBorrows(r, f, ((ConditionalExpr*)n)->condition, changed);
        ScanBorrows(r, f, ((ConditionalExpr*)n)->thenExpr, changed);
        ScanBorrows(r, f, ((ConditionalExpr*)n)->elseExpr, changed);
        return;
    case NodeArrayInit:
    {
        ArrayInitExpr* ai = (ArrayInitExpr*)n;

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            Node* e = (Node*)VecGet(&ai->elements, i);

            if (!ai->repeatCount)
            {
                ConsumeIfTaken(r, f, e, changed); /* `{ v; n }` copies its value */
            }
            ScanBorrows(r, f, e, changed);
        }
        ScanBorrows(r, f, ai->repeatCount, changed);
        return;
    }
    case NodeStructInit:
        for (size_t i = 0; i < ((StructInitExpr*)n)->fields.count; i++)
        {
            Node* v = ((StructInitField*)VecGet(&((StructInitExpr*)n)->fields, i))->value;
            ConsumeIfTaken(r, f, v, changed);
            ScanBorrows(r, f, v, changed);
        }
        return;
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;
        bool isMethod = c->calleeBase != NULL;

        ScanBorrows(r, f, c->calleeBase, changed);

        for (size_t i = 0; i < c->args.count; i++)
        {
            ScanBorrows(r, f, (Node*)VecGet(&c->args, i), changed);
        }

        if (!c->callee)
        {
            for (size_t i = 0; i < c->args.count; i++)
            {
                ConsumeParam(BorrowRoot(f, (Node*)VecGet(&c->args, i)), changed);
            }
            return;
        }

        Node* arg0 = c->args.count > 0 ? (Node*)VecGet(&c->args, 0) : NULL;

        if (!isMethod && (IsArrayMutator(c->callee) || strcmp(c->callee, "drop") == 0
                          || strcmp(c->callee, "addressof") == 0))
        {
            ConsumeParam(BorrowRoot(f, arg0), changed);

            if (strcmp(c->callee, "array_push") == 0 && c->args.count > 1)
            {
                ConsumeIfTaken(r, f, (Node*)VecGet(&c->args, 1), changed);
            }
            return;
        }

        if (!isMethod && (strcmp(c->callee, "copy") == 0 || strcmp(c->callee, "sizeof") == 0
                          || strcmp(c->callee, "alignof") == 0 || strcmp(c->callee, "load") == 0
                          || strcmp(c->callee, "substring") == 0))
        {
            return; /* only read */
        }

        if (isMethod)
        {
            ScanBorrowArg(r, f, c, c->calleeBase, 0, true, changed);
        }

        for (size_t i = 0; i < c->args.count; i++)
        {
            ScanBorrowArg(r, f, c, (Node*)VecGet(&c->args, i), isMethod ? i + 1 : i, isMethod, changed);
        }
        return;
    }
    default:
        return;
    }
}

static void InferBorrowedParams(Resolver* r)
{
    Module* mod = r->m_mod;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);
        bool eligible = f->body && !f->isExtern && !f->isExport && !f->isDelegate;

        for (size_t j = 0; j < f->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&f->params, j);
            p->borrowCandidate = eligible && p->mod == ModNone && !p->isVarargRest && !p->isReturn
                                 && BorrowCandidateType(r, &p->type);
        }
    }

    bool changed = true;

    for (int round = 0; changed && round < 64; round++)
    {
        changed = false;

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);

            if (f->body)
            {
                ScanBorrows(r, f, f->body, &changed);
            }
        }
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);

        for (size_t j = 0; j < f->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&f->params, j);

            if (p->borrowCandidate)
            {
                p->borrowCandidate = false;
                p->inferredBorrow = true;
                p->mod = ModRef;
                p->type.isConst = true;
            }
        }
    }
}

/* Marks every growing `ref T[]` parameter, to a fixpoint: passing a parameter on to a callee that
   grows it grows it too. */
static void MarkGrowableArrayParams(Module* mod)
{
    bool changed = true;

    for (int round = 0; changed && round < 64; round++)
    {
        changed = false;

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);
            ScanGrowth(mod, f, f->body, &changed);
        }
    }
}

static bool IsGrowableArrayParam(const Resolver* r, const char* name)
{
    const ParamDecl* p = RefArrayParamNamed(r->m_currentFunction, name);

    return p && p->growsArray;
}

/* A growing `ref T[]` parameter needs a heap array: reject stack views at the call site. */
static void CheckGrowableArrayArgs(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope)
{
    for (size_t i = 0; i < best->params.count && i < c->args.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&best->params, i);

        if (!p->growsArray)
        {
            continue;
        }

        Node* arg = (Node*)VecGet(&c->args, i);
        const TypeName* argType = InferType(r, arg, scope);
        const char* why = NULL;

        if (arg->kind == NodeArrayInit)
        {
            why = "an array literal";
        }
        else if (argType && TypeNameIsFixedArray(argType))
        {
            why = "a fixed-size array";
        }
        else if (arg->kind == NodeIdent && r->m_currentFunction)
        {
            for (size_t j = 0; j < r->m_currentFunction->params.count; j++)
            {
                const ParamDecl* own = (const ParamDecl*)VecGet((Vec*)&r->m_currentFunction->params, j);

                if (own->isVarargRest && strcmp(own->name, ((IdentExpr*)arg)->name) == 0)
                {
                    why = "a varargs rest array";
                }
            }
        }

        if (why)
        {
            DiagErrorFmt(r->m_diag, arg->range,
                         "'%s' grows or rebinds its parameter '%s', so it needs a dynamic array variable, not %s",
                         best->methodName ? best->methodName : best->name, p->name, why);
        }
    }
}

//-- Conditional expressions.

static void CheckBoolCondition(Resolver* r, Node* cond, StrMap* scope);
static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType);

static int NumericRank(PrimitiveType p)
{
    switch (p)
    {
    case PrimDouble: return 9;
    case PrimFloat: return 8;
    case PrimULong: return 7;
    case PrimLong: return 6;
    case PrimUInt: return 5;
    case PrimInt: return 4;
    case PrimUShort: return 3;
    case PrimShort: return 2;
    case PrimByte: return 1;
    case PrimSByte: return 1;
    default: return 0;
    }
}

/* The type of `c ? a : b`: two string literals (or cstrings) give a borrowed `cstring`; otherwise the
   branch types must agree (or one convert to the other, numbers widening). Owning branches are
   rejected: one path's value would have to be dropped and the other's moved. */
static const TypeName* ConditionalType(Resolver* r, ConditionalExpr* ce, StrMap* scope, bool diagnose)
{
    const TypeName* lt = InferType(r, ce->thenExpr, scope);
    const TypeName* rt = InferType(r, ce->elseExpr, scope);

    if (!lt || !rt)
    {
        return lt ? lt : rt;
    }

    bool lCs = ce->thenExpr->kind == NodeStrLiteral || TypeIsCString(&r->m_registry, lt->name);
    bool rCs = ce->elseExpr->kind == NodeStrLiteral || TypeIsCString(&r->m_registry, rt->name);

    if (lCs && rCs)
    {
        return InternTypeName(r, "cstring");
    }

    if (AliasIsOwning(r, lt) || AliasIsOwning(r, rt))
    {
        if (diagnose)
        {
            DiagErrorFmt(r->m_diag, ce->base.range,
                         "'?:' branches cannot be owning values ('%s' and '%s'); use if/else", lt->name, rt->name);
        }
        return lt;
    }

    if (strcmp(lt->name, rt->name) == 0)
    {
        return lt;
    }

    if (IsNumeric(lt->primitiveType) && IsNumeric(rt->primitiveType) && lt->primitiveType != PrimBool
        && rt->primitiveType != PrimBool)
    {
        return NumericRank(lt->primitiveType) >= NumericRank(rt->primitiveType) ? lt : rt;
    }

    if (IsAssignableType(r, lt, rt))
    {
        return lt;
    }

    if (IsAssignableType(r, rt, lt))
    {
        return rt;
    }

    if (diagnose)
    {
        DiagErrorFmt(r->m_diag, ce->base.range, "'?:' branches have incompatible types '%s' and '%s'", lt->name,
                     rt->name);
    }

    return lt;
}

//-- `extern delegate` types.

/* A delegate is called with the extern ABI, so its signature follows the extern rules. */
static void CheckDelegateDecl(Resolver* r, const FunctionDecl* f)
{
    if (f->hasReturnParam)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "delegate '%s' cannot take a 'return' parameter", f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&f->params, i);
        const char* leaf = TypeRegistryResolveAlias(&r->m_registry, p->type.name);

        if (p->mod == ModNone && !p->type.isBox && !p->type.isOptional && !TypeNameIsDynamicArray(&p->type)
            && IsDefinedStruct(&r->m_registry, leaf))
        {
            DiagErrorFmt(r->m_diag, p->base.range,
                         "delegate parameter '%s' can't take struct '%s' by value; declare it 'const ref %s'", p->name,
                         p->type.name, p->type.name);
        }

        if (p->isVarargRest)
        {
            DiagErrorFmt(r->m_diag, p->base.range, "delegate '%s' cannot have a typed rest parameter", f->name);
        }
    }

    const char* ret = TypeRegistryResolveAlias(&r->m_registry, f->returnType.name);

    if (IsDefinedStruct(&r->m_registry, ret) || TypeNameIsDynamicArray(&f->returnType) || f->returnType.isOptional)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "delegate '%s' cannot return '%s' by value", f->name,
                     f->returnType.name);
    }
}

//-- `@export` functions: the plain C ABI.

/* A value that crosses a C call in one register: a number, bool, enum, handle or cstring. */
static bool IsCAbiScalar(Resolver* r, const TypeName* t)
{
    if (!t || !t->name || TypeNameIsOwning(t) || t->isArray)
    {
        return false;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->m_registry, t->name);

    if (IsHandleType(&r->m_registry, leaf) || TypeIsCString(&r->m_registry, leaf))
    {
        return true;
    }

    PrimitiveType prim = GetPrimitiveType(leaf);

    return prim != PrimNone && prim != PrimVoid && prim != PrimString && IsNameSimdVector(leaf) == 0;
}

static void CheckExportDecl(Resolver* r, const FunctionDecl* f)
{
    if (f->isExtern || !f->body)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "'@export' function '%s' needs a body (it is defined here, not by the host)",
                     f->name);
        return;
    }

    if (f->isVariadic)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "'@export' function '%s' cannot be variadic", f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&f->params, i);
        const char* leaf = TypeRegistryResolveAlias(&r->m_registry, p->type.name);

        /* `ref T` of a plain struct is a `T*` in C. */
        bool plainStructRef = p->mod == ModRef && IsDefinedStruct(&r->m_registry, leaf)
                              && !TypeRegistryIsOwningStruct(&r->m_registry, leaf);

        if (!plainStructRef && (p->mod == ModRef || !IsCAbiScalar(r, &p->type)))
        {
            DiagErrorFmt(r->m_diag, p->base.range,
                         "'@export' parameter '%s' of type '%s' has no C ABI; use numbers, bools, enums, handles, "
                         "cstring, or 'ref' to a plain struct",
                         p->name, p->type.name);
        }
    }

    if (GetPrimitiveType(f->returnType.name) != PrimVoid && !IsCAbiScalar(r, &f->returnType))
    {
        DiagErrorFmt(r->m_diag, f->base.range,
                     "'@export' function '%s' cannot return '%s'; return a number, bool, enum, handle or cstring",
                     f->name, f->returnType.name);
    }
}

//-- Memory intrinsics: raw `ptr` access for FFI.

static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType);

/* The only function named `name` visible from `fileId`; NULL (with *ambiguous set) when overloaded. */
static FunctionDecl* FindSingleFunction(Resolver* r, const char* name, uint16_t fileId, bool* ambiguous)
{
    FunctionDecl* found = NULL;
    *ambiguous = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&r->m_mod->functions, i);

        if (strcmp(f->name, name) != 0 || !FunctionVisibleTo(f, fileId))
        {
            continue;
        }

        if (found)
        {
            *ambiguous = true;
            return NULL;
        }

        found = f;
    }

    return found;
}

/* `addressof(f)` names a function, not a variable: a bare identifier that is no local/global. */
static bool IsAddressOfFunctionArg(Resolver* r, const CallExpr* c, StrMap* scope)
{
    if (!c->callee || strcmp(c->callee, "addressof") != 0 || c->args.count != 1)
    {
        return false;
    }

    const Node* arg = (const Node*)VecGet((Vec*)&c->args, 0);

    if (arg->kind != NodeIdent || StrMapGet(scope, ((const IdentExpr*)arg)->name))
    {
        return false;
    }

    return ModuleHasFunctionNamed(r, ((const IdentExpr*)arg)->name, c->base.range.fileId);
}

/* A `T` that load/store/sizeof may move as raw bytes: no owning parts. */
static bool CheckPlainDataTypeArg(Resolver* r, const CallExpr* c, const TypeName* t)
{
    if (!t || !t->name)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' needs a type argument (e.g. '%s<int>(...)')", c->callee,
                     c->callee);
        return false;
    }

    if (AliasIsOwningValue(r, t) || TypeRegistryIsOwningStruct(&r->m_registry, TypeRegistryResolveAlias(&r->m_registry, t->name)))
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' cannot move owning type '%s' as raw memory", c->callee, t->name);
        return false;
    }

    if (TypeRegistryIsOpaque(&r->m_registry, t->name) && !IsHandleType(&r->m_registry, t->name))
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' cannot use incomplete type '%s'", c->callee, t->name);
        return false;
    }

    if (GetPrimitiveType(t->name) == PrimVoid)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' cannot use 'void'", c->callee);
        return false;
    }

    return true;
}

/* A pointer operand: any handle (they all convert to `ptr`). */
static bool CheckPtrOperand(Resolver* r, const CallExpr* c, Node* arg, StrMap* scope)
{
    const TypeName* t = InferType(r, arg, scope);

    if (!t || !IsHandleType(&r->m_registry, t->name))
    {
        DiagErrorFmt(r->m_diag, arg->range, "'%s' expects a 'ptr' (or other handle), not '%s'", c->callee,
                     t ? t->name : "");
        return false;
    }

    return true;
}

static const TypeName* MemoryBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    (void)scope;

    if (!c->callee)
    {
        return NULL;
    }

    if (strcmp(c->callee, "addressof") == 0)
    {
        return InternTypeName(r, STRATA_PTR_TYPE);
    }

    if (strcmp(c->callee, "sizeof") == 0 || strcmp(c->callee, "alignof") == 0)
    {
        return InternTypeName(r, "ulong");
    }

    if (strcmp(c->callee, "load") == 0)
    {
        return c->typeArg;
    }

    return NULL; /* store: void */
}

/* addressof(x) -> ptr        the address of a variable, field or element (a box: its cell), or of an
                              extern / `@export` function
   sizeof<T>() / alignof<T>() -> ulong
   load<T>(p) -> T            reads a T at p
   store(p, v) / store<T>(p, v) writes v (as T) at p */
static bool ResolveMemoryBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    /* These names are not reserved: a module's own function of the same name wins. */
    if (!c->callee || ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
    {
        return false;
    }

    const char* name = c->callee;

    if (strcmp(name, "addressof") == 0)
    {
        if (c->args.count != 1 || c->typeArg)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'addressof' expects 1 argument, got %zu", c->args.count);
            return true;
        }

        Node* arg0 = (Node*)VecGet(&c->args, 0);

        if (IsAddressOfFunctionArg(r, c, scope))
        {
            bool ambiguous = false;
            const char* fnName = ((IdentExpr*)arg0)->name;
            FunctionDecl* fn = FindSingleFunction(r, fnName, c->base.range.fileId, &ambiguous);

            if (!fn)
            {
                DiagErrorFmt(r->m_diag, arg0->range, "cannot take the address of overloaded function '%s'", fnName);
                return true;
            }

            if ((!fn->isExtern && !fn->isExport) || fn->typeParam)
            {
                DiagErrorFmt(r->m_diag, arg0->range,
                             "cannot take the address of '%s': only extern and '@export' functions have a C ABI",
                             fnName);
                return true;
            }

            c->addressOfFunction = fn;
            c->isIntrinsicCall = true;
            return true;
        }

        while (arg0->kind == NodeCast)
        {
            arg0 = ((CastExpr*)arg0)->operand;
        }

        if (!IsLValueNode(arg0))
        {
            DiagError(r->m_diag, arg0->range, "'addressof' expects a variable, field, element or function");
            return true;
        }

        if (arg0->kind == NodeMember && IsFatPseudoProperty(r, InferType(r, ((MemberExpr*)arg0)->base_node, scope),
                                                             ((MemberExpr*)arg0)->member))
        {
            DiagErrorFmt(r->m_diag, arg0->range, "cannot take the address of '.%s'", ((MemberExpr*)arg0)->member);
            return true;
        }

        c->isIntrinsicCall = true;
        return true;
    }

    if (strcmp(name, "sizeof") == 0 || strcmp(name, "alignof") == 0)
    {
        if (c->args.count != 0)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'%s' takes no arguments: write '%s<T>()'", name, name);
            return true;
        }

        if (c->typeArg && TypeNameIsDynamicArray(c->typeArg))
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'%s' of a dynamic array is not a fixed size", name);
            return true;
        }

        if (!c->typeArg)
        {
            CheckPlainDataTypeArg(r, c, NULL);
            return true;
        }

        const TypeName* t = c->typeArg;
        bool known = t->primitiveType != PrimNone || t->isArray || t->isBox || t->isOptional
                     || TypeRegistryIsUserType(&r->m_registry, t->name);

        if (t->primitiveType == PrimVoid || !known)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "unknown type '%s'", c->typeArg->name);
            return true;
        }

        c->isIntrinsicCall = true;
        return true;
    }

    if (strcmp(name, "load") == 0)
    {
        if (c->args.count != 1)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'load' expects 1 argument, got %zu", c->args.count);
            return true;
        }

        if (!CheckPlainDataTypeArg(r, c, c->typeArg) || !CheckPtrOperand(r, c, (Node*)VecGet(&c->args, 0), scope))
        {
            return true;
        }

        c->isIntrinsicCall = true;
        return true;
    }

    if (strcmp(name, "store") == 0)
    {
        if (c->args.count != 2)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "'store' expects 2 arguments (ptr, value), got %zu", c->args.count);
            return true;
        }

        Node* value = (Node*)VecGet(&c->args, 1);
        const TypeName* valueType = InferType(r, value, scope);
        const TypeName* t = c->typeArg ? c->typeArg : valueType;

        if (!CheckPlainDataTypeArg(r, c, t) || !CheckPtrOperand(r, c, (Node*)VecGet(&c->args, 0), scope))
        {
            return true;
        }

        if (c->typeArg && valueType && !IsAssignableType(r, c->typeArg, valueType))
        {
            DiagErrorFmt(r->m_diag, value->range, "cannot store '%s' as '%s'", valueType->name, c->typeArg->name);
            return true;
        }

        if (!c->typeArg)
        {
            c->typeArg = (TypeName*)arena_dup(r->m_arena, valueType, sizeof(TypeName));
        }

        c->isIntrinsicCall = true;
        return true;
    }

    return false;
}

/* `array_remove(a, i)` / `array_swap_remove(a, i)`: drop element i, keeping order / filling the hole
   with the last element. */
static bool IsArrayRemoveName(const char* callee)
{
    return strcmp(callee, "array_remove") == 0 || strcmp(callee, "array_swap_remove") == 0;
}

static FunctionDecl* FindSingleFunction(Resolver* r, const char* name, uint16_t fileId, bool* ambiguous);

/* A fixed array passed where an extern takes a `ptr` decays to its first element, as in C:
   `snprintf(buffer, ...)` with `byte[32] buffer`. The argument is wrapped in that `(ptr)` cast. */
static void DecayFixedArgsToPtr(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee || c->calleeBase)
    {
        return;
    }

    bool ambiguous = false;
    FunctionDecl* fd = FindSingleFunction(r, c->callee, c->base.range.fileId, &ambiguous);

    if (!fd || !fd->isExtern || fd->isDelegate)
    {
        return;
    }

    for (size_t i = 0; i < c->args.count && i < fd->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&fd->params, i);
        const char* paramType = TypeRegistryResolveAlias(&r->m_registry, param->type.name);

        if (param->mod != ModNone || !paramType || strcmp(paramType, STRATA_PTR_TYPE) != 0)
        {
            continue;
        }

        Node* arg = (Node*)VecGet(&c->args, i);
        const TypeName* argType = InferType(r, arg, scope);

        if (!argType || !TypeNameIsFixedArray(argType))
        {
            continue;
        }

        CastExpr* cast = AST_NEW(r->m_arena, CastExpr);
        cast->base.kind = NodeCast;
        cast->base.range = arg->range;
        cast->type = *InternTypeName(r, STRATA_PTR_TYPE);
        cast->operand = arg;
        cast->textKind = CastFixedToPtr;
        VecSet(&c->args, i, cast);
    }
}

/* Result type of array_push/pop/resize/remove, or NULL if not one. */
static const TypeName* ArrayBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return NULL;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    bool isResize = strcmp(c->callee, "array_resize") == 0 || IsArrayRemoveName(c->callee)
                    || strcmp(c->callee, "array_reserve") == 0;
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

    return InternTypeName(r, isPush ? "uint" : "void");
}

static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType);
static bool IsVectorAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType,
                                   const char* resolvedTarget, const char* resolvedValue);

static Node* ApplyBracedStructTarget(Resolver* r, Node* node, const TypeName* target);

/* An EMPTY braced value for a `T?` target is the canonical empty optional
   (zero-fill), not a T value missing its owning fields. */
static bool IsEmptyBracedForOptional(const Node* n, const TypeName* target)
{
    bool empty = (n->kind == NodeArrayInit && ((const ArrayInitExpr*)n)->elements.count == 0)
                 || (n->kind == NodeStructInit && ((const StructInitExpr*)n)->fields.count == 0);

    return empty && target && target->isOptional;
}

/* Types nested bare-braced field values from their field's declared type,
   then requires every OWNING field to be covered. For literals whose type
   arrives only AFTER the context-free pre-resolution pass (call/ctor/
   `array_push` args), whose full checks would otherwise never run. */
static void CheckStructInitFields(Resolver* r, Node* n);

// True for `arr[i]` reads (casts unwrapped); the array keeps ownership.
static bool IsArrayElementBorrow(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return n && n->kind == NodeIndex;
}

/* A string literal contextually coerces to `cstring` (a `.rodata` pointer).
   A `string`-typed value never converts to `cstring` implicitly. */
static bool IsStrLiteralForCString(const Resolver* r, const TypeName* targetType, const Node* valueNode)
{
    return targetType && valueNode && valueNode->kind == NodeStrLiteral
           && TypeIsCString(&r->m_registry, targetType->name);
}

// Array builtins (push/pop/resize); marks the call pseudo. True if handled.
static bool ResolveArrayBuiltin(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (!c->callee)
    {
        return false;
    }

    bool isPush = strcmp(c->callee, "array_push") == 0;
    /* `array_reserve(a, n)` grows the capacity like a resize would, without touching the elements. */
    bool isResize = strcmp(c->callee, "array_resize") == 0 || strcmp(c->callee, "array_reserve") == 0;
    bool isPop = strcmp(c->callee, "array_pop") == 0;
    bool isRemove = IsArrayRemoveName(c->callee);

    if (!isPush && !isResize && !isPop && !isRemove)
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

    // Push/pop/resize mutate the array in place.
    const char* constName = ConstRootName(r, arg0, scope);

    if (constName)
    {
        DiagErrorFmt(r->m_diag, arg0->range, "'%s' cannot modify '%s'; it is immutable", c->callee, constName);
        return true;
    }

    // A `ref T[]` param may be a borrowed stack view (fixed-array slice):
    // growing/shrinking can reallocate, which would free the caller's storage.
    if ((isPush || isResize) && arg0->kind == NodeIdent && StrMapGet(&r->m_refArrayParams, ((IdentExpr*)arg0)->name)
        && !IsGrowableArrayParam(r, ((IdentExpr*)arg0)->name))
    {
        DiagErrorFmt(r->m_diag, arg0->range,
                     "'%s' cannot grow '%s' (a ref array param may borrow the caller's stack storage); "
                     "copy it into a local array first",
                     c->callee, ((IdentExpr*)arg0)->name);
        return true;
    }

    // The buffer may be reallocated or elements shifted: refs to elements die.
    InvalidateRefLocals(r, MovableBoxSourceKey(r, arg0));

    // Length may change: drop `[recv.length]` facts (resize/pop also drop below).
    {
        const char* lenRecvKey = MovableBoxSourceKey(r, arg0);

        if (lenRecvKey)
        {
            InvalidateIndexVar(r, KeyRoot(r->m_arena, lenRecvKey));
        }
    }

    // Resize/pop/remove drop element facts (removal moves elements between slots); push keeps them.
    if (isResize || isPop || isRemove)
    {
        const char* recvKey = MovableBoxSourceKey(r, arg0);

        if (recvKey)
        {
            ClearNullableFacts(r, recvKey);
        }
    }

    if (isResize || isRemove)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);

        const TypeName* sizeType = InferType(r, arg1, scope);

        if (sizeType && (!IsNumeric(sizeType->primitiveType) || sizeType->primitiveType == PrimBool))
        {
            DiagErrorFmt(r->m_diag, arg1->range, "'%s' %s must be an integer, not '%s'", c->callee,
                         isRemove ? "index" : (strcmp(c->callee, "array_reserve") == 0 ? "capacity" : "size"),
                         sizeType->name);
        }
    }
    else if (isPush)
    {
        Node* arg1 = (Node*)VecGet(&c->args, 1);

        const TypeName* elemType = TypeNameArrayElem(unwrapped);

        /* A bare `{...}` value takes the element's shape here — the literal
           would otherwise never be typed: no field checks run, and codegen
           zero-fills it (a null `^T` owning field crashes at first deref). */
        bool bracedValue = (arg1->kind == NodeArrayInit && !((ArrayInitExpr*)arg1)->elementType)
                           || (arg1->kind == NodeStructInit && !((StructInitExpr*)arg1)->typeName);

        Node* typedArg = ApplyBracedStructTarget(r, arg1, elemType);

        if (typedArg != arg1)
        {
            VecSet(&c->args, 1, typedArg);
        }

        if (bracedValue && typedArg->kind == NodeStructInit && ((StructInitExpr*)typedArg)->typeName
            && !IsEmptyBracedForOptional(typedArg, elemType))
        {
            CheckStructInitFields(r, typedArg);
        }

        const TypeName* valueType = InferType(r, typedArg, scope);

        if (valueType && elemType && !IsAssignableType(r, elemType, valueType)
            && !IsStrLiteralForCString(r, elemType, typedArg))
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
        if (valueType && !valueType->isRc && AliasIsOwningValue(r, valueType) && IsArrayElementBorrow(arg1))
        {
            DiagErrorFmt(r->m_diag, arg1->range,
                         "cannot push '%s' read from an array element - it would be owned by two arrays; "
                         "move it into a variable first, or push a copy() of it",
                         valueType->name);
        }

        /* Deref check before the move tracking below clears the pushed optional's fact. */
        CheckCallArgOptionalDerefs(r, c, scope);

        // Push moves owning values in (boxes/strings/arrays AND owning
        // structs — a `Rec` value pushed into `Rec[]` transfers ownership);
        // optional sources end up empty.
        if (valueType && AliasIsOwningValue(r, valueType))
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

    c->isIntrinsicCall = true;

    return true;
}

// Bare `...` accepts scalars, strings, and handles (not structs/arrays).
static bool IsCVarargScalarish(Resolver* r, const TypeName* type)
{
    if (!type)
    {
        return false;
    }

    if (IsNumeric(type->primitiveType) || TypeIsString(&r->m_registry, type->name))
    {
        return true;
    }

    if (type->isArray || IsSimdVector(type->primitiveType))
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
    if (!type || type->primitiveType == PrimVoid)
    {
        return false;
    }

    /* `cstring` crosses as `const char*` — always vararg-compatible. */
    if (TypeIsCString(&r->m_registry, type->name))
    {
        return true;
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

        // ...and refs into globals may dangle after any call that could reach them.
        if (!best->isExtern)
        {
            InvalidateRefLocals(r, gd->name);
        }
    }

    // A callee given a path by mutable ref (or taking it) may grow, shrink or rebind it.
    {
        size_t named = NamedParamCount(best);

        for (size_t j = 0; j < c->args.count && j < named; j++)
        {
            const ParamDecl* p = (ParamDecl*)VecGet(&best->params, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            if ((p->mod == ModRef && !p->type.isConst) || (p->mod == ModNone && AliasIsOwning(r, &p->type)))
            {
                InvalidateRefLocals(r, MovableBoxSourceKey(r, arg));
            }
        }
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

                    // Anything under the borrowed path may be rewritten (`h.f`, `a[0]`).
                    ClearNestedNullableFacts(r, refKey);

                    // A `T?` source unwrapped into a plain `ref T` param only
                    // borrows the pointee - the callee cannot rebind (or
                    // empty) the caller's slot, so the non-empty fact
                    // survives. A `ref T?` param crosses the SLOT itself,
                    // which the callee may rebind: its fact dies.
                    if (rp->type.isOptional)
                    {
                        ClearNullableFacts(r, refKey);
                    }
                }
            }
        }
        else if (typedRest && best->params.count > 0)
        {
            const ParamDecl* rp = (ParamDecl*)VecGet(&best->params, best->params.count - 1);

            if (rp->mod == ModRef && !rp->type.isConst)
            {
                if (arg->kind == NodeIdent)
                {
                    InvalidateIndexVar(r, ((IdentExpr*)arg)->name);
                }

                // A mutable ref rest element may be rewritten like a named ref arg.
                ClearNullableFacts(r, MovableBoxSourceKey(r, arg));
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
            DiagErrorFmt(r->m_diag, arg->range,
                         "'%s' is immutable and cannot be passed to non-const 'ref' parameter '%s' of '%s'", constName,
                         rp->name ? rp->name : "", best->name);
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

static void CheckStructInitFields(Resolver* r, Node* n)
{
    StructInitExpr* init = (StructInitExpr*)n;

    const StructType* structType = TypeRegistryFind(&r->m_registry, init->typeName);

    if (!structType || structType->fields.count == 0 || TypeRegistryIsOpaque(&r->m_registry, init->typeName))
    {
        return;
    }

    size_t fieldCount = structType->fields.count;
    bool* covered = (bool*)arena_alloc(r->m_arena, fieldCount * sizeof(bool));
    size_t positionalSeen = 0;

    for (size_t i = 0; i < init->fields.count; i++)
    {
        StructInitField* field = (StructInitField*)VecGet(&init->fields, i);

        size_t idx = (size_t)-1;
        FieldDecl* fd = NULL;

        if (field->name && field->name[0] != '\0')
        {
            int named = TypeRegistryFieldIndex(&r->m_registry, init->typeName, field->name);

            if (named >= 0)
            {
                idx = (size_t)named;
                fd = (FieldDecl*)VecGet((Vec*)&structType->fields, idx);
            }
        }
        else if (positionalSeen < fieldCount)
        {
            idx = positionalSeen++;
            fd = (FieldDecl*)VecGet((Vec*)&structType->fields, idx);
        }
        else
        {
            positionalSeen++; /* too many initializers: reported elsewhere */
        }

        if (field->value)
        {
            /* Type a nested bare-braced value from the field's declared type
               (`{ {} }`); already-typed values pass through unchanged. */
            if (fd && (field->value->kind == NodeArrayInit || field->value->kind == NodeStructInit))
            {
                field->value = ApplyBracedStructTarget(r, field->value, &fd->type);
            }

            if (field->value->kind == NodeStructInit && ((StructInitExpr*)field->value)->typeName)
            {
                bool emptyOptional = fd && fd->type.isOptional && ((StructInitExpr*)field->value)->fields.count == 0;

                if (!emptyOptional)
                {
                    CheckStructInitFields(r, field->value);
                }
            }
        }

        if (idx < fieldCount)
        {
            covered[idx] = true;
        }
    }

    for (size_t f = 0; f < fieldCount; f++)
    {
        FieldDecl* fd = (FieldDecl*)VecGet((Vec*)&structType->fields, f);

        /* Every OWNING field (`^T`, owning struct, `string`) must be
           initialized; optionals and dynamic arrays may stay empty (a T? is
           null, a zero-filled T[] is the canonical empty {null, 0} fat
           struct). */
        bool mustInit = !fd->type.isOptional && !TypeNameIsDynamicArray(&fd->type)
                        && (AliasIsOwning(r, &fd->type) || TypeRegistryIsOwningStruct(&r->m_registry, fd->type.name));

        if (mustInit && !covered[f])
        {
            /* Suggest the optional spelling of the inner type. A `^string`
               field's optional is spelled `string?`. */
            const TypeName* inner = TypeNameBoxInner(&fd->type);
            const char* optSpelling = inner ? inner->name : fd->type.name;

            DiagErrorFmt(r->m_diag, init->base.range,
                         "owning field '%s' of struct '%s' must be initialized "
                         "(declare it '%s?' if it may be empty)",
                         fd->name, init->typeName, optSpelling);
        }
    }
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

/* Method lookup walks base handles: a `handle Player : Entity` sees impl Entity methods. */
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
    const PrimitiveType primitive = GetPrimitiveType(baseName);

    if (!IsScalarPseudoType(primitive))
    {
        return false;
    }

    if (!ScalarPseudoConst(primitive, m->member, NULL, NULL, NULL))
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
            && (strcmp(p0->type.name, handleName) == 0 || HandleExtendsFrom(&r->m_registry, handleName, p0->type.name)))
        {
            VecPushFront(&c->args, c->calleeBase);
        }
    }

    c->calleeBase = NULL;
    c->isIntrinsicCall = false;
    c->callee = method->name; /* qualified extern symbol, e.g. Camera_GetFOV */

    return false;
}

/* Synthesizes the extern declaration a property accessor refers to. When a
   function with that name already exists (e.g. a flat extern), its signature
   is validated against the accessor shape instead. */
static void SynthesizeAccessor(Module* mod, Arena* arena, const char* symbol, const TypeName* propType,
                               const char* handleName, bool selfByRef, bool isSetter, SourceRange range,
                               DiagnosticEngine* diag)
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
            DiagErrorFmt(diag, range, "property %s '%s' must take (%s self%s); found %zu parameter(s)",
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
            if (match->returnType.primitiveType != PrimVoid)
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

    /* A struct receiver crosses as a pointer, like any extern struct param; a getter only reads it. */
    ParamDecl* self = AST_NEW(arena, ParamDecl);
    self->base.kind = NodeParam;
    self->base.range = range;
    self->mod = selfByRef ? ModRef : ModNone;
    self->type = TypeNameLeaf(arena_strdup(arena, handleName));
    self->type.isConst = selfByRef && !isSetter;
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
typedef enum
{
    EnumEvalOk = 0,
    EnumEvalNotConst,       /* not a constant integer expression (incl. div-by-zero, bad shift) */
    EnumEvalNegForUnsigned, /* a unary minus in an expression for an unsigned underlying */
    EnumEvalForwardRef,     /* references a member that isn't resolved yet (self/forward) */
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

            if (ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)me->base_node)->name), me->member, &intVal, NULL,
                                  &isFloat))
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
                                     "enum member '%s' may not be negative for unsigned underlying type '%s'", m->name,
                                     underlying);
                        continue;
                    }

                    if (m->value > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %llu does not fit in '%s'", m->name,
                                     (unsigned long long)m->value, underlying);
                        continue;
                    }

                    next = m->value + 1;
                }
                else
                {
                    if (next > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %llu does not fit in '%s'", m->name,
                                     (unsigned long long)next, underlying);
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
                        m->value = m->isNegative
                                       ? ((v == (uint64_t)0x8000000000000000ULL) ? v : (uint64_t)(-(int64_t)v))
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
            DiagErrorFmt(diag, impl->base.range, "impl type '%s' is not a declared struct or handle", impl->handleName);
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
            if (prop->returnType.primitiveType == PrimVoid)
            {
                DiagErrorFmt(diag, prop->range, "property '%s' may not have type 'void'", prop->name);
                continue;
            }

            if (TypeNameIsDynamicArray(&prop->returnType))
            {
                DiagErrorFmt(diag, prop->range, "property '%s' may not have a dynamic array type ('%s')", prop->name,
                             prop->returnType.name);
                continue;
            }

            const char* targetLeaf = TypeRegistryResolveAlias(registry, impl->handleName);
            bool selfByRef = IsDefinedStruct(registry, targetLeaf) || IsIncompleteStruct(registry, targetLeaf);

            if (prop->getterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->getterSymbol, &prop->returnType, impl->handleName, selfByRef,
                                   false, prop->range, diag);
            }

            if (prop->setterSymbol)
            {
                SynthesizeAccessor(mod, arena, prop->setterSymbol, &prop->returnType, impl->handleName, selfByRef,
                                   true, prop->range, diag);
            }
        }
    }
}

/* True when some visible overload of `name` takes a `ref T[]` param whose
   element matches the fixed array's — the arg can cross as a borrowed
   stack-fat view instead of being rejected wholesale. */
static bool OverloadTakesFixedView(Resolver* r, const char* name, const TypeName* fixedType, int fileId)
{
    const TypeName* argElem = TypeNameArrayElem(fixedType);

    if (!argElem)
    {
        return false;
    }

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        const FunctionDecl* f = (const FunctionDecl*)VecGet(&r->m_mod->functions, i);

        if (strcmp(f->name, name) != 0 || !FunctionVisibleTo(f, fileId))
        {
            continue;
        }

        for (size_t j = 0; j < f->params.count; j++)
        {
            const ParamDecl* p = (const ParamDecl*)VecGet(&f->params, j);

            if (p->isVarargRest || p->mod != ModRef || !TypeNameIsDynamicArray(&p->type))
            {
                continue;
            }

            const TypeName* paramElem = TypeNameArrayElem(&p->type);

            if (paramElem && strcmp(paramElem->name, argElem->name) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

/* Stamps a braced literal and its nested rows with element types, top-down.
   Call args are resolved BEFORE the overload is known (so the generic
   inheritance walk saw a NULL elementType); the chosen param supplies the
   types here. */
static void StampArrayInitElemTypes(ArrayInitExpr* ai, const TypeName* elem)
{
    if (!elem || ai->elementType)
    {
        return;
    }

    ai->elementType = elem;

    if (!TypeNameIsArray(elem))
    {
        return;
    }

    const TypeName* inner = TypeNameArrayElem(elem);

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        Node* child = (Node*)VecGet(&ai->elements, i);

        if (child && child->kind == NodeArrayInit && !((ArrayInitExpr*)child)->elementType)
        {
            StampArrayInitElemTypes((ArrayInitExpr*)child, inner);
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
    for (const TypeName* at = type; at; at = (at->isBox || at->isOptional) ? at->inner : (at->isArray ? at->elem : NULL))
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
    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        FunctionDecl* candidate = (FunctionDecl*)VecGet(&r->m_mod->functions, i);

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
    for (size_t i = 0; i < r->m_mod->genericInstances.count; i++)
    {
        FunctionDecl* existing = (FunctionDecl*)VecGet(&r->m_mod->genericInstances, i);

        if (existing->genericOf == generic && strcmp(existing->typeArg, structName) == 0)
        {
            return existing;
        }
    }

    FunctionDecl* instance = AST_NEW(r->m_arena, FunctionDecl);
    *instance = *generic;
    instance->typeParam = NULL;
    instance->genericOf = generic;
    instance->typeArg = arena_strdup(r->m_arena, structName);
    instance->mangledName = arena_format(r->m_arena, "%s<%s>", generic->name, structName);
    VecInit(&instance->params);

    for (size_t i = 0; i < generic->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&generic->params, i);
        ParamDecl* copy = AST_NEW(r->m_arena, ParamDecl);
        *copy = *param;

        if (IsTypeParamName(generic, &param->type))
        {
            TypeName substituted = TypeNameParse(r->m_arena, instance->typeArg);
            substituted.isConst = param->type.isConst;
            substituted.range = param->type.range;
            copy->type = substituted;
        }

        VecPush(&instance->params, copy);
    }

    VecPush(&r->m_mod->genericInstances, instance);

    return instance;
}

/* What `extern<T>` allows: T only as a whole `ref` / `const ref` parameter type, never in the
   return type, and no variadic or `return` parameters. */
static void CheckGenericExternDecl(Resolver* r, const FunctionDecl* f)
{
    if (TypeRegistryIsUserType(&r->m_registry, f->typeParam) || GetPrimitiveType(f->typeParam) != PrimNone)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "type parameter '%s' of '%s' has the same name as a type", f->typeParam,
                     f->name);
    }

    if (TypeMentionsParam(f, &f->returnType))
    {
        DiagErrorFmt(r->m_diag, f->base.range, "generic extern '%s' can't return its type parameter '%s'", f->name,
                     f->typeParam);
    }

    if (f->isVariadic || f->hasReturnParam)
    {
        DiagErrorFmt(r->m_diag, f->base.range, "generic extern '%s' can't be variadic or take a 'return' parameter",
                     f->name);
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        const ParamDecl* param = (const ParamDecl*)VecGet(&f->params, i);

        if (TypeMentionsParam(f, &param->type) && (!IsTypeParamName(f, &param->type) || param->mod != ModRef))
        {
            DiagErrorFmt(r->m_diag, param->base.range,
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
            DiagErrorFmt(r->m_diag, c->base.range, "'%s' is not a generic extern; it takes no type argument",
                         c->callee);
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
        DiagErrorFmt(r->m_diag, c->base.range, "can't infer '%s' for '%s'; name it explicitly: '%s<Type>(...)'",
                     generic->typeParam, generic->name, generic->name);
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->m_registry, typeArg->name);
    const StructType* structType = (typeArg->isArray || typeArg->isBox || typeArg->isOptional)
                                       ? NULL
                                       : TypeRegistryFind(&r->m_registry, leaf);

    if (!structType || structType->opaque || structType->isTypeAlias)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "type argument '%s' for '%s' must be a struct type", typeArg->name,
                     generic->name);
        return true;
    }

    if (structType->owning)
    {
        DiagErrorFmt(r->m_diag, c->base.range,
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
        DiagErrorFmt(r->m_diag, c->base.range, "'%s' takes %zu argument(s) but %zu were given", generic->name,
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
            DiagErrorFmt(r->m_diag, arg->range, "argument %zu of '%s' is passed by non-const 'ref' and must be a variable",
                         i + 1, generic->name);
        }
        else if (argType && !IsAssignableType(r, &param->type, argType))
        {
            DiagErrorFmt(r->m_diag, arg->range, "argument %zu of '%s' has type '%s' but '%s' was expected", i + 1,
                         generic->name, argType->name, param->type.name);
        }
    }

    CheckCallArgOptionalDerefs(r, c, scope);
    CheckConstRefArgs(r, instance, c, scope);
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
        const StructType* st = baseType ? TypeRegistryFind(&r->m_registry, baseType->name) : NULL;
        int idx = st ? TypeRegistryFieldIndex(&r->m_registry, baseType->name, c->callee) : -1;

        if (idx < 0)
        {
            return false;
        }

        const FieldDecl* field = (const FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx);

        if (!TypeRegistryDelegate(&r->m_registry, field->type.name))
        {
            return false;
        }

        MemberExpr* m = AST_NEW(r->m_arena, MemberExpr);
        m->base.kind = NodeMember;
        m->base.range = c->base.range;
        m->base_node = c->calleeBase;
        m->member = c->callee;
        c->delegateCallee = (Node*)m;
        c->calleeBase = NULL;
    }
    else
    {
        const TypeName* varType = (const TypeName*)StrMapGet(scope, c->callee);

        if (!varType || !TypeRegistryDelegate(&r->m_registry, varType->name))
        {
            return false;
        }

        IdentExpr* id = AST_NEW(r->m_arena, IdentExpr);
        id->base.kind = NodeIdent;
        id->base.range = c->base.range;
        id->name = c->callee;
        c->delegateCallee = (Node*)id;
    }

    ResolveExpr(r, c->delegateCallee, scope);

    const TypeName* t = InferType(r, c->delegateCallee, scope);
    c->delegateDecl = t ? TypeRegistryDelegate(&r->m_registry, t->name) : NULL;

    if (!c->delegateDecl)
    {
        return false;
    }

    c->callee = c->delegateDecl->mangledName;
    return true;
}

static void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope)
{
    /* Calls through delegate values pick their one signature directly. */
    bool delegateCall = DetectDelegateCall(r, c, scope);

    if (delegateCall)
    {
        goto resolve_candidates;
    }

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

    if (ResolveGenericExternCall(r, c, scope))
    {
        return;
    }

    /* A whole fixed-size array *local* cannot be passed as a call argument —
       only its elements (`values[i]`, `values.length`) — UNLESS some overload
       borrows it as a `ref T[]` slice/view (a stack fat is built at the call
       site; the callee never drops the binding). Locals are named by a bare
       identifier, so an Ident arg with a fixed type is always a local;
       field-sourced fixed arrays (`b.data`) keep the extern `^T` decay below. */
    DecayFixedArgsToPtr(r, c, scope);

    {
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
                DiagErrorFmt(r->m_diag, arg->range,
                             "cannot pass fixed-size array '%s' of type '%s' as an argument; "
                             "pass its elements instead (e.g. '%s[0]')",
                             ((IdentExpr*)arg)->name, argType->name, ((IdentExpr*)arg)->name);
                fixedArgError = true;
            }
        }

        if (fixedArgError)
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

            bool bracedArg = (arg->kind == NodeArrayInit && !((ArrayInitExpr*)arg)->elementType)
                             || (arg->kind == NodeStructInit && !((StructInitExpr*)arg)->typeName);

            // A braced arg takes the field's struct/array shape.
            Node* resolvedArg = ApplyBracedStructTarget(r, arg, &fd->type);

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
                const TypeName* ftArr = ft->isOptional ? ft->inner : ft;

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

            if (AliasIsOwningValue(r, &fd->type))
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

    const bool isValidIntrinsic = ResolveIntrinsicCall(r, c, scope);
    if (isValidIntrinsic)
    {
        return;
    }

resolve_candidates:;
    /* A delegate call has exactly one candidate: its signature. */
    Vec delegatePool;
    VecInit(&delegatePool);

    if (delegateCall)
    {
        VecPush(&delegatePool, (void*)c->delegateDecl);
    }

    const Vec* pool = delegateCall ? &delegatePool : &r->m_mod->functions;

    /* Call already resolved (e.g. warmup); reuse cached decl and rerun move tracking. */
    if (c->resolvedDecl)
    {
        const FunctionDecl* best = c->resolvedDecl;

        /* A cached `@private` decl must not leak across files. */
        if (!FunctionVisibleTo(best, c->base.range.fileId))
        {
            c->resolvedDecl = NULL;
        }
        else
        {
            /* Facts before moves. */
            CheckCallArgOptionalDerefs(r, c, scope);
            CheckConstRefArgs(r, best, c, scope);
            CheckGrowableArrayArgs(r, best, c, scope);
            TrackCallArgMoves(r, best, c);
            free(delegatePool.items);

            return;
        }
    }

    bool found = delegateCall;

    for (size_t i = 0; !found && i < pool->count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet((Vec*)pool, i);

        if (strcmp(functionDecl->name, c->callee) == 0 && FunctionVisibleTo(functionDecl, c->base.range.fileId))
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        DiagErrorFmt(r->m_diag, c->base.range, "unknown function '%s'", c->callee);
        free(delegatePool.items);
        return;
    }

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
                     && !(TypeNameIsOwning(argType) && !paramType->isOptional) && !TypeNameIsFixedArray(argType))
            {
                viable = false;
                break;
            }
            else if (arg->kind == NodeStrLiteral && TypeIsCString(&r->m_registry, paramType->name))
            {
                /* String literal contextually coerces to `cstring`. */
                score += 1;
            }
            else if (TypeIsCString(&r->m_registry, argType->name) && TypeIsString(&r->m_registry, paramType->name))
            {
                /* `cstring` copies into a `string` param (heap copy). */
                score += 1;
            }
            else if (!mutableRef && IsImplicitScalarConversion(paramType->primitiveType, argType->primitiveType))
            {
                score += 1;
            }
            else if (IsSimdVector(argType->primitiveType) && IsSimdVector(paramType->primitiveType))
            {
                // Same rule as assignment: no implicit lane-count or element-type change.
                if (IsVectorAssignableType(r, paramType, argType,
                                           TypeRegistryResolveAlias(&r->m_registry, paramType->name),
                                           TypeRegistryResolveAlias(&r->m_registry, argType->name)))
                {
                    score += 1;
                }
                else
                {
                    viable = false;
                    break;
                }
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
            else if (!mutableRef && paramType->isBox && argType->isOptional && paramType->isRc == argType->isRc
                     && TypeNameBoxInner(paramType) && TypeNameBoxInner(argType)
                     && strcmp(TypeNameBoxInner(paramType)->name, TypeNameBoxInner(argType)->name) == 0)
            {
                /* Smart cast: a proven `T?` / `rc<T>?` passes as `^T` / `rc<T>` (proof checked by
                   CheckCallArgOptionalDerefs once the overload is picked). */
                score += 1;
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
            else if (!isTail && param->mod == ModRef && TypeNameIsDynamicArray(paramType)
                     && TypeNameIsFixedArray(argType))
            {
                /* Fixed → `ref T[]` slice/view: the fixed inline storage is
                   borrowed as a stack fat {&fixed[0], N, N} — no copy, no
                   allocation; a ref callee mutates elements, never drops or
                   frees the binding (ref params are exempt from teardown). */
                const TypeName* argElem = TypeNameArrayElem(argType);
                const TypeName* paramElem = TypeNameArrayElem(paramType);

                if (argElem && paramElem && strcmp(argElem->name, paramElem->name) == 0)
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

    free(delegatePool.items);

    if (!best)
    {
        if (delegateCall)
        {
            DiagErrorFmt(r->m_diag, c->base.range, "arguments do not match delegate '%s'", c->delegateDecl->name);
        }
        else
        {
            DiagErrorFmt(r->m_diag, c->base.range, "no matching overload for '%s' with %zu argument(s)", c->callee,
                         c->args.count);
        }
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
            if (arg->kind == NodeArrayInit && TypeNameIsDynamicArray(&param->type)
                && !((ArrayInitExpr*)arg)->elementType)
            {
                if (best->isExtern && param->mod != ModRef)
                {
                    DiagErrorFmt(r->m_diag, arg->range,
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

    CheckCallArgOptionalDerefs(r, c, scope);
    CheckConstRefArgs(r, best, c, scope);
    CheckGrowableArrayArgs(r, best, c, scope);
    TrackCallArgMoves(r, best, c);
}

static const TypeName* VectorConstructBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (IsNameSimdVector(c->callee) != 0)
    {
        return InternTypeName(r, c->callee);
    }

    return NULL;
}

static const TypeName* VectorDotBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    return InternTypeName(r, "float");
}

static const TypeName* VectorCrossBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    Node* a0 = (Node*)VecGet(&c->args, 0);
    const TypeName* a0Type = InferType(r, a0, scope);

    const char* resolved = TypeRegistryResolveAlias(&r->m_registry, a0Type ? a0Type->name : "");
    if (GetNameSimdVectorLanes(resolved) == 2)
    {
        return InternTypeName(r, "float");
    }

    return a0Type;
}

static const TypeName* VectorReduceBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    return InternTypeName(r, "float");
}

// -- Instrinsics

typedef struct IntrinsicTypeDefinition
{
    const char* name;
    const TypeName* (*typeFunc)(Resolver*, CallExpr*, StrMap*);
    bool (*resolveFunc)(Resolver*, CallExpr*, StrMap*);
} IntrinsicTypeDefinition;

// NOTE: Make sure there are matching definitions for any new intrinsics added at `EmitIntrinsicCall()` in
// LLVMModuleBuilder.c.
static const IntrinsicTypeDefinition intrinsics[] = {
    /* Array calls */
    {"array_push",   ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_pop",    ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_resize", ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_remove", ArrayBuiltinType,           ResolveArrayBuiltin    },
    {"array_reserve", ArrayBuiltinType,          ResolveArrayBuiltin    },
    {"array_swap_remove", ArrayBuiltinType,      ResolveArrayBuiltin    },

    /* Memory */
    {"copy",         CopyBuiltinType,            ResolveCopyBuiltin     },
    {"drop",         DropBuiltinType,            ResolveDropBuiltin     },
    {"addressof",    MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"sizeof",       MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"alignof",      MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"load",         MemoryBuiltinType,          ResolveMemoryBuiltin   },
    {"store",        MemoryBuiltinType,          ResolveMemoryBuiltin   },

    /* Strings */
    {"substring",    SubstringBuiltinType,       ResolveSubstringBuiltin},

    /* Vectors */
    {"float2",       VectorConstructBuiltinType, ResolveVectorConstruct },
    {"float3",       VectorConstructBuiltinType, ResolveVectorConstruct },
    {"float4",       VectorConstructBuiltinType, ResolveVectorConstruct },

    {"dot",          VectorDotBuiltinType,       ResolveVector2Arg      },
    {"cross",        VectorCrossBuiltinType,     ResolveVector2Arg      },
    {"reduce",       VectorReduceBuiltinType,    ResolveVector1Arg      },
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

static const TypeName* InferIntrinsicType(Resolver* r, CallExpr* c, StrMap* scope)
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

        /* Values that don't fit a non-negative signed i32 are 64-bit, as is any `l` literal. */
        if (lit->value > 0x7FFFFFFFULL || lit->isLong)
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

        /* The builtin `null` constant (a variable named `null` shadows it). */
        if (!t && IsNullConstantName(((IdentExpr*)n)->name))
        {
            return InternTypeName(r, STRATA_NULL_TYPE);
        }

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
            PrimitiveType lp = lt ? lt->primitiveType : PrimNone;

            if (IsEnumFlagsOp(r, b->op, lt, rt))
            {
                return lt;
            }

            if (IsStringConcat(r, b->op, lt, rt))
            {
                return InternTypeName(r, "string");
            }
            PrimitiveType rp = rt ? rt->primitiveType : PrimNone;

            /* Box/optional operands deref to their inner for arithmetic
               (mirrors the ResolveExpr operand check). Same-shape SIMD
               vectors keep the vector type: the scalar ladder below would
               report a bogus `int` and break overload matching for valid
               vector arguments. */
            const char* ln2 = lt && lt->isBox && lt->inner ? lt->inner->name : lt ? lt->name : "";
            const char* rn2 = rt && rt->isBox && rt->inner ? rt->inner->name : rt ? rt->name : "";

            if (SameResolvedType(r, ln2, rn2) && IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0)
            {
                return InternTypeName(r, TypeRegistryResolveAlias(&r->m_registry, ln2));
            }

            if (lp == PrimDouble || rp == PrimDouble)
            {
                return InternTypeName(r, "double");
            }

            if (lp == PrimFloat || rp == PrimFloat)
            {
                return InternTypeName(r, "float");
            }

            if (lp == PrimULong || rp == PrimULong)
            {
                return InternTypeName(r, "ulong");
            }

            if (lp == PrimLong || rp == PrimLong)
            {
                return InternTypeName(r, "long");
            }

            if (lp == PrimUInt || rp == PrimUInt)
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
    case NodeConditional:
    {
        ConditionalExpr* ce = (ConditionalExpr*)n;

        return ce->resultType ? ce->resultType : ConditionalType(r, ce, scope, false);
    }
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
            PrimitiveType primitive = GetPrimitiveType(baseName);

            if (IsScalarPseudoType(primitive) && ScalarPseudoConst(primitive, m->member, NULL, NULL, NULL))
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

        /* SIMD lane read / swizzle: `v.x` is a float, `v.xy` a float2, `v.xyz` a float3. */
        if (IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, baseType->name)) != 0)
        {
            size_t lanes = strlen(m->member);
            bool swizzle = lanes >= 1 && lanes <= 4;

            for (size_t i = 0; swizzle && i < lanes; i++)
            {
                swizzle = m->member[i] >= 'w' && m->member[i] <= 'z';
            }

            if (swizzle)
            {
                static const char* const swizzleTypes[5] = {NULL, "float", "float2", "float3", "float4"};
                return InternTypeName(r, swizzleTypes[lanes]);
            }
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

        /* Array/string pseudo-properties: `.length` and `.cap` read the fat's
           u32 fields (uint). Fixed arrays expose a compile-time `.length`
           only — they have no capacity (inline storage). */
        if (IsFatPseudoProperty(r, baseType, m->member))
        {
            return InternTypeName(r, strcmp(m->member, "data") == 0 ? STRATA_PTR_TYPE : "uint");
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

        // Infer the type for intrinsic calls
        const TypeName* intrinsicType = InferIntrinsicType(r, c, scope);
        if (intrinsicType != NULL)
        {
            return intrinsicType;
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

        if (baseType && TypeIsCString(&r->m_registry, baseType->name))
        {
            // `cs[i]` is a bounds-checked byte read (strlen is the length).
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

/**
 * @brief Checks to see if a vector is assignable to another vector type.
 */
static bool IsVectorAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType,
                                   const char* resolvedTarget, const char* resolvedValue)
{
    // Ethan's SIMD vector rules:
    //
    // - Identical vector types are of course valid.
    //
    // - Differing vector sizes cannot be implicitly converted. That would need to be explicitly cast or be build as the
    // destination vector type (e.g. `float2 y = value.xy`), to prevent unintended logic errors.
    //
    // - Source values that are numeric (but not vectors!) CAN be assigned to a vector type. They will be automatically
    // broadcasted to a vector of the destination's type. `bool` is not numeric here.

    const int destLanes = IsNameSimdVector(resolvedTarget);
    const int srcLanes = IsNameSimdVector(resolvedValue);

    // Check identical vector types
    if (destLanes && srcLanes && srcLanes == destLanes)
    {
        if (strcmp(targetType->name, valueType->name) != 0)
        {
            // They are differing types (e.g. int4 x = float4(1.0)).
            return false;
        }

        return true;
    }

    // Check vector dest and scalar numeric src
    if (destLanes && (srcLanes == 0 && IsNameNumeric(resolvedValue) && GetPrimitiveType(resolvedValue) != PrimBool))
    {
        // If the dest type is an alias, it is not assignable this way.
        if (TypeRegistryIsTypeAlias(&r->m_registry, targetType->name))
        {
            return false;
        }

        return true;
    }

    return false;
}

static bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType)
{
    /* A `cstring` is a `const char*`: it passes wherever a raw `ptr` is expected. */
    if (targetType && valueType && TypeIsCString(&r->m_registry, valueType->name)
        && strcmp(targetType->name, STRATA_PTR_TYPE) == 0)
    {
        return true;
    }

    /* `cstring` copies into `string` (heap copy at the use site). The
       reverse is never implicit: only a string literal initializes a
       `cstring` (handled at each use site via IsStrLiteralForCString). */
    if (targetType && valueType && TypeIsCString(&r->m_registry, valueType->name)
        && TypeIsString(&r->m_registry, targetType->name))
    {
        return true;
    }

    if (targetType && TypeIsCString(&r->m_registry, targetType->name))
    {
        return valueType && SameResolvedType(r, valueType->name, targetType->name);
    }

    if (targetType && TypeNameIsOwning(targetType))
    {
        /* An rc cell carries a count a plain box does not: rc and ^T / T? never convert into each other. */
        if (valueType && targetType->isRc != valueType->isRc && (valueType->isBox || valueType->isOptional))
        {
            return false;
        }

        const TypeName* inner = TypeNameBoxInner(targetType);

        // `T?` and `^T` share an ABI; they convert when inners match.
        const TypeName* valueInner = TypeNameBoxInner(valueType);

        if (targetType->isOptional && valueType->isBox && inner && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        /* Smart cast: a `T?` (or `rc<T>?`) goes where `^T` (or `rc<T>`) is wanted once flow typing
           proves it non-empty; CheckOptionalDeref demands the proof at each use site. */
        if (!targetType->isOptional && targetType->isBox && valueType->isOptional && inner && valueInner)
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

    // Numerics convert freely, bool excluded (TODO: require casts for lossy narrowing).
    if (IsImplicitScalarConversion(targetType->primitiveType, valueType->primitiveType))
    {
        return true;
    }

    const char* resolvedTarget = TypeRegistryResolveAlias(&r->m_registry, targetType->name);
    const char* resolvedValue = TypeRegistryResolveAlias(&r->m_registry, valueType->name);

    // Check SIMD vectors for assignability
    if (IsVectorAssignableType(r, targetType, valueType, resolvedTarget, resolvedValue))
    {
        return true;
    }

    const TypeName* valueInner = TypeNameBoxInner(valueType);

    return HandleExtendsFrom(&r->m_registry, valueType->name, targetType->name)
           || (valueInner && strcmp(valueInner->name, targetType->name) == 0);
}

/* True for `{}`: an empty braced value, which default-constructs. */
static bool IsEmptyBraced(const Node* n)
{
    return (n->kind == NodeArrayInit && ((const ArrayInitExpr*)n)->elements.count == 0
            && !((const ArrayInitExpr*)n)->repeatCount)
           || (n->kind == NodeStructInit && ((const StructInitExpr*)n)->fields.count == 0);
}

/* Whether all-zero bytes are a valid `t`: what `{ {}; count }` and array_resize fill with. Empty
   strings, arrays and optionals and zeroed scalars are; a `^T` or `rc<T>` (which must point at
   something) is not, nor is a struct holding one. */
static bool ZeroIsValid(Resolver* r, const TypeName* t, int depth)
{
    if (!t || depth > 32 || t->isOptional || TypeNameIsDynamicArray(t) || TypeIsString(&r->m_registry, t->name))
    {
        return true;
    }

    if (t->isBox)
    {
        return false;
    }

    if (t->isArray && t->length >= 0)
    {
        return ZeroIsValid(r, t->elem, depth + 1);
    }

    const char* name = TypeRegistryResolveAlias(&r->m_registry, t->name);
    const StructType* st = name ? TypeRegistryFind(&r->m_registry, name) : NULL;

    if (!st || TypeRegistryIsOpaque(&r->m_registry, name))
    {
        return true;
    }

    for (size_t f = 0; f < st->fields.count; f++)
    {
        const FieldDecl* fd = (const FieldDecl*)VecGet((Vec*)&st->fields, f);

        if (!ZeroIsValid(r, &fd->type, depth + 1))
        {
            return false;
        }
    }

    return true;
}

/* `{ value; count }` for a dynamic array: the count is any integer; the value is evaluated once and
   each element is a copy of it (a `{}` value instead default-constructs each element). The value is
   read, not moved, so a variable used as the value stays usable. */
static void ResolveArrayRepeat(Resolver* r, ArrayInitExpr* ai, StrMap* scope)
{
    ResolveExpr(r, ai->repeatCount, scope);

    const TypeName* countType = InferType(r, ai->repeatCount, scope);

    if (countType && (!IsNumeric(countType->primitiveType) || countType->primitiveType == PrimBool
                      || countType->primitiveType == PrimFloat || countType->primitiveType == PrimDouble))
    {
        DiagErrorFmt(r->m_diag, ai->repeatCount->range, "the count in '{ value; count }' must be an integer, not '%s'",
                     countType->name);
    }

    Node* value = (Node*)VecGet(&ai->elements, 0);
    const TypeName* elem = ai->elementType;

    /* `{ {}; count }` zero-fills each element: the default value, built in place. */
    if (IsEmptyBraced(value))
    {
        if (elem && !ZeroIsValid(r, elem, 0))
        {
            DiagErrorFmt(r->m_diag, value->range,
                         "'{ {}; count }' fills each element with its zero value, which is not a valid '%s' (a "
                         "'^T' or 'rc<T>' in it must point at something); give a value to copy instead",
                         elem->name);
        }
        return;
    }

    if (elem && (value->kind == NodeArrayInit || value->kind == NodeStructInit))
    {
        value = ApplyBracedStructTarget(r, value, elem);
        VecSet(&ai->elements, 0, value);
    }

    ResolveExpr(r, value, scope);

    if (!elem)
    {
        return;
    }

    const TypeName* valueType = InferType(r, value, scope);

    if (valueType && !IsAssignableType(r, elem, valueType) && !IsStrLiteralForCString(r, elem, value)
        && !IsEmptyBraced(value))
    {
        DiagErrorFmt(r->m_diag, value->range, "a '%s' cannot fill an array of '%s'", valueType->name, elem->name);
    }

    if (!IsEmptyBraced(value) && AliasIsOwningValue(r, elem) && !elem->isRc && TypeNameHasDrop(&r->m_registry, elem))
    {
        DiagErrorFmt(r->m_diag, value->range,
                     "'%s' has a Drop method, so it cannot be copied into each element; write '{ {}; count }' "
                     "to default-construct them instead",
                     elem->name);
    }

    CheckOptionalDeref(r, value, valueType, elem, value->range);
}

/* `ref T x = path;` / `const ref T x = path;`: an alias of the storage at `path`, which must be a
   variable, field or element of type T (or a `^T` / `rc<T>` / proven `T?` holding one). Nothing is
   moved or copied; the alias cannot be rebound or moved from, and dies (see InvalidateRefLocals) once
   what it refers to may have moved. */
static void ScopeDeclare(Resolver* r, StrMap* scope, const char* name, void* typePtr, SourceRange range);

static void ResolveRefLocal(Resolver* r, VarDeclStmt* vd, StrMap* scope)
{
    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
    ClearBoxSubtree(r, vd->name);
    ClearNullableFacts(r, vd->name);
    InvalidateIndexVar(r, vd->name);

    if (!vd->init)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "ref local '%s' must be bound to a variable, field or element", vd->name);
        return;
    }

    ResolveExpr(r, vd->init, scope);

    const char* key = MovableBoxSourceKey(r, vd->init);

    if (!IsLValueNode(vd->init) || !key)
    {
        DiagErrorFmt(r->m_diag, vd->init->range,
                     "ref local '%s' must be bound to a variable, field or element, not a temporary", vd->name);
        return;
    }

    const TypeName* initType = InferType(r, vd->init, scope);

    if (!initType)
    {
        return;
    }

    const TypeName* inner = (initType->isBox || initType->isOptional) ? TypeNameBoxInner(initType) : NULL;
    bool exact = SameResolvedType(r, initType->name, vd->type.name);
    bool through = !exact && !vd->type.isBox && !vd->type.isOptional && inner
                   && SameResolvedType(r, inner->name, vd->type.name);

    if (!exact && !through)
    {
        DiagErrorFmt(r->m_diag, vd->init->range, "'ref %s %s' cannot be bound to a '%s'", vd->type.name, vd->name,
                     initType->name);
        return;
    }

    if (through)
    {
        // Reaching into a `T?` reads through it: it must be proven set.
        CheckOptionalDeref(r, vd->init, initType, &vd->type, vd->init->range);
    }

    const char* constRoot = ConstRootName(r, vd->init, scope);

    if (constRoot && !vd->type.isConst)
    {
        DiagErrorFmt(r->m_diag, vd->init->range,
                     "'%s' is immutable, so a mutable ref cannot be bound into it; use 'const ref'", constRoot);
    }

    vd->refThroughBox = through;
    StrMapPut(&r->m_refLocals, vd->name, (void*)CanonicalRefKey(r, key));
    StrMapPut(&r->m_refThroughBox, vd->name, through ? (void*)1 : NULL);

    // Like a `ref` param: it does not own what it aliases, so it is never moved from or rebound.
    if (AliasIsOwning(r, &vd->type) || AliasIsOwningValue(r, &vd->type))
    {
        StrMapPut(&r->m_refBoxParams, vd->name, (void*)1);
    }
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

    r->m_curScope = scope;

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

        if (!varType && IsNullConstantName(ident->name))
        {
            return;
        }

        if (!varType)
        {
            DiagErrorFmt(r->m_diag, ident->base.range, "unknown variable '%s'", ident->name);
        }
        else if (StrMapGet(&r->m_refLocals, ident->name) && IsBoxMoved(r, ident->name))
        {
            DiagErrorFmt(r->m_diag, ident->base.range,
                         "'%s' may dangle: what it refers to may have moved, been reallocated or freed since it "
                         "was bound (bind it again after the change)",
                         ident->name);
        }
        else if (AliasIsOwning(r, varType))
        {
            // Whole use rejects moved values; member bases only fully-moved ones.
            bool blocked = asMemberBase ? IsBoxMoved(r, ident->name) : IsBoxUnusable(r, ident->name);

            if (blocked)
            {
                if (IsBoxPartiallyMoved(r, ident->name))
                {
                    DiagPoisoned(r, ident->base.range, ident->name);
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
    {
        UnaryExpr* u = (UnaryExpr*)n;

        ResolveExpr(r, u->operand, scope);

        /* `-bool` and `~bool` compute i1 garbage (signed 1-bit neg/not).
           Only `!` is defined for bools. */
        if (u->op == UnNeg || u->op == UnBitNot)
        {
            const TypeName* ot = InferType(r, u->operand, scope);

            if (ot && IsBoolType(&r->m_registry, ot))
            {
                DiagErrorFmt(r->m_diag, u->base.range, "invalid operand to unary operator ('%s')", ot->name);
            }
        }

        // `!` is logical only: `!x` on a number is an implicit == 0 test.
        if (u->op == UnNot)
        {
            const TypeName* ot = InferType(r, u->operand, scope);

            if (ot && !IsBoolContextType(&r->m_registry, ot))
            {
                DiagErrorFmt(r->m_diag, u->base.range, "invalid operand to unary operator ('%s')", ot->name);
            }
        }

        return;
    }
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        ResolveExpr(r, b->lhs, scope);

        /* `a? && a.x`: the right side runs only when the left is true (`||`: when it is false). */
        if (b->op == BinLogicAnd || b->op == BinLogicOr)
        {
            ResolveUnderCondition(r, b->rhs, b->lhs, b->op == BinLogicAnd, scope);
        }
        else
        {
            ResolveExpr(r, b->rhs, scope);
        }

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
            const TypeName* li = lt && lt->isBox && lt->inner ? lt->inner : lt;
            const TypeName* ri = rt && rt->isBox && rt->inner ? rt->inner : rt;
            const char* ln2 = li ? li->name : ln;
            const char* rn2 = ri ? ri->name : rn;

            bool numericPair = li && ri && IsNumeric(li->primitiveType) && IsNumeric(ri->primitiveType);
            bool vectorPair
                = SameResolvedType(r, ln2, rn2) && IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0;

            /* Bool is not numeric: `true + true` must not compute i1
               wrap-around (1+1 = 0). */
            bool boolOperand = (lt && IsBoolType(&r->m_registry, lt)) || (rt && IsBoolType(&r->m_registry, rt));

            /* `&`, `|`, `^` combine flags of one enum type (`Usage.A | Usage.B`). */
            bool flagsPair = IsEnumFlagsOp(r, b->op, lt, rt);

            /* `a + b` on strings / cstrings / literals concatenates into a new string. */
            bool concatPair = IsStringConcat(r, b->op, lt, rt);

            if (lt && rt && (!numericPair || boolOperand) && !vectorPair && !flagsPair && !concatPair)
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
            bool lCStr = lt && TypeIsCString(&r->m_registry, ln);
            bool rCStr = rt && TypeIsCString(&r->m_registry, rn);

            /* A string literal on one side of a `cstring` comparison coerces
               to `cstring` (contextual), so `cs == "lit"` is well-typed. */
            if (lCStr && rString && b->rhs->kind == NodeStrLiteral)
            {
                rString = false;
                rCStr = true;
            }

            if (rCStr && lString && b->lhs->kind == NodeStrLiteral)
            {
                lString = false;
                lCStr = true;
            }

            /* Ordering a bool is meaningless (i1 compares with signed 1-bit
               semantics: `true < false` computes garbage). Only `==`/`!=`
               are defined for bools. */
            bool eqOp = (b->op == BinEqEq || b->op == BinNotEq);
            bool boolOperand = (lt && IsBoolType(&r->m_registry, lt)) || (rt && IsBoolType(&r->m_registry, rt));

            /* Comparing a `T?` reads through it. Only `==`/`!=` with an
               optional LEFT side and a box/optional right side is
               null-aware (empty == empty); every other comparison derefs the
               optional, so it must be proven non-empty first. `T[]?`
               compares as the array it wraps (empty is a zero-length fat). */
            {
                bool lOpt = lt && lt->isOptional && !TypeIsComparableAggregate(&r->m_registry, lt);
                bool rOpt = rt && rt->isOptional && !TypeIsComparableAggregate(&r->m_registry, rt);
                bool lNullAware = eqOp && lOpt && rt && (rt->isOptional || rt->isBox);
                bool rNullAware = eqOp && lOpt;

                if (lOpt && !lNullAware && !IsPathNonEmpty(r, MovableBoxSourceKey(r, b->lhs)))
                {
                    DiagOptionalReadError(r, b->lhs->range, MovableBoxSourceKey(r, b->lhs), lt->name);
                }

                if (rOpt && !rNullAware && !IsPathNonEmpty(r, MovableBoxSourceKey(r, b->rhs)))
                {
                    DiagOptionalReadError(r, b->rhs->range, MovableBoxSourceKey(r, b->rhs), rt->name);
                }
            }

            /* `flag == 1` would compare through an implicit numeric <-> bool
               conversion; both sides of a bool comparison must be bool. */
            const TypeName* lScalar = TypeNameBoxInner(lt) ? TypeNameBoxInner(lt) : lt;
            const TypeName* rScalar = TypeNameBoxInner(rt) ? TypeNameBoxInner(rt) : rt;
            bool mixedBool = lScalar && rScalar
                             && IsBoolType(&r->m_registry, lScalar) != IsBoolType(&r->m_registry, rScalar);

            if ((boolOperand && !eqOp) || mixedBool)
            {
                DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
                break;
            }

            /* `string == cstring` (either order) compares the text, like `string == string`. */
            if (eqOp && ((lString && rCStr) || (rString && lCStr)))
            {
                break;
            }

            if (lString != rString)
            {
                DiagErrorFmt(r->m_diag, b->base.range, "cannot compare 'string' with '%s'", lString ? rn : ln);
                break;
            }

            if (lString)
            {
                break;
            }

            /* `cstring` compares by content like `string`. */
            if (lCStr != rCStr)
            {
                DiagErrorFmt(r->m_diag, b->base.range, "cannot compare 'cstring' with '%s'", lCStr ? rn : ln);
                break;
            }

            if (lCStr)
            {
                if (!eqOp)
                {
                    DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln,
                                 rn);
                }

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

            if (lt && rt
                && (!TypeIsTriviallyComparable(&r->m_registry, lt) || !TypeIsTriviallyComparable(&r->m_registry, rt)))
            {
                DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
            }

            break;
        }
        case BinLogicAnd:
        case BinLogicOr:
        {
            // Logical operators take bools only; no implicit numeric truthiness.
            const TypeName* lt = InferType(r, b->lhs, scope);
            const TypeName* rt = InferType(r, b->rhs, scope);

            if (lt && rt && (!IsBoolContextType(&r->m_registry, lt) || !IsBoolContextType(&r->m_registry, rt)))
            {
                DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", lt->name,
                             rt->name);
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

        CheckConstAssign(r, a->target, a->base.range, scope);

        /* Storing into a path frees or rebinds what was there: refs reaching through it die. */
        InvalidateRefLocals(r, MovableBoxSourceKey(r, a->target));

        /* `cstring` is constant storage: element writes (plain or compound)
           are rejected. Whole-slot rebinding (`cs = ...`) is fine. */
        if (a->target->kind == NodeIndex)
        {
            Node* base = a->target;

            while (base && base->kind == NodeIndex)
            {
                base = ((IndexExpr*)base)->base_node;
            }

            if (base)
            {
                const TypeName* baseType = InferType(r, base, scope);

                if (baseType && TypeIsCString(&r->m_registry, baseType->name))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign into a 'cstring' (a constant string)");
                    return;
                }
            }
        }

        /* `.length` / `.cap` on arrays and strings are read-only views of
           the fat (fixed `.length` is a compile-time constant). */
        if (a->target->kind == NodeMember)
        {
            MemberExpr* fatM = (MemberExpr*)a->target;

            if (IsFatPseudoProperty(r, InferType(r, fatM->base_node, scope), fatM->member))
            {
                DiagErrorFmt(r->m_diag, a->base.range,
                             "cannot assign to '.%s' (read-only; arrays grow via array_push/array_resize)",
                             fatM->member);
                return;
            }
        }

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

                if (vt && !IsAssignableType(r, &prop->returnType, vt)
                    && !IsStrLiteralForCString(r, &prop->returnType, a->value))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to property '%s' of type '%s'", vt->name,
                                 tm->member, prop->returnType.name);
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

        /* A `ref T[]` param may be a borrowed stack view (fixed-array slice):
           rebinding drops/frees the old buffer — the caller's storage. */
        if (tt && a->target->kind == NodeIdent && TypeNameIsDynamicArray(tt)
            && StrMapGet(&r->m_refArrayParams, ((IdentExpr*)a->target)->name)
            && !IsGrowableArrayParam(r, ((IdentExpr*)a->target)->name))
        {
            DiagErrorFmt(r->m_diag, a->base.range,
                         "'%s' cannot be reassigned as it is not owned because it is bound as a ref array param; "
                         "mutate its elements instead",
                         ((IdentExpr*)a->target)->name);

            return;
        }

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
            && (a->target->kind == NodeIdent || a->target->kind == NodeMember || a->target->kind == NodeIndex))
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

                /* A `ref ^T` is the caller's box: assign contents, not the slot. A `ref` to a `T?`,
                   `rc<T>` or `rc<T>?` (or a string) is the caller's slot: `=` drops what it held and
                   stores the new value there, so out-params work. */
                bool slotRef = tt->isOptional || tt->isRc || TypeIsString(&r->m_registry, tt->name)
                               || TypeNameIsDynamicArray(tt);

                if (StrMapGet(&r->m_refBoxParams, targetName) && !slotRef)
                {
                    DiagErrorFmt(r->m_diag, a->base.range,
                                 "'%s' cannot be reassigned as it is not owned because it is bound as a ref; "
                                 "assign its inner value instead",
                                 targetName);

                    return;
                }

                // Rebinding drops the fact; `=` restores it for provably non-empty values.
                const char* movedValueKey = MovableBoxSourceKey(r, a->value);
                bool valueProvesNonEmpty
                    = !optionalTarget || (vt && !vt->isOptional) || (movedValueKey && IsPathNonEmpty(r, movedValueKey));

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

                /* An owning-struct inner (`^Rec b; b = arr[i];`) REPLACES the
                   cell contents, so the RHS value moves in: the source is
                   consumed (codegen drops the old contents, then constructs
                   the new value into the cell). */
                if (a->op == AssignSet && inner && vt && TypeRegistryIsOwningStruct(&r->m_registry, inner->name)
                    && AliasIsOwningValue(r, vt))
                {
                    const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                    if (movedValueKey)
                    {
                        MoveBoxIdent(r, movedValueKey, a->base.range);
                    }
                }

                // New contents: facts on the box's sub-paths (`h.f`) are stale.
                ClearNestedNullableFacts(r, targetName);
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
                bool valueProvesNonEmpty
                    = !vt || !vt->isOptional || (movedValueKey && IsPathNonEmpty(r, movedValueKey));

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
            // A plain `=` overwrites an element: a moved-out slot may be refilled.
            r->m_indexWriteTarget = a->target->kind == NodeIndex && a->op == AssignSet;
            ResolveExpr(r, a->target, scope);
            r->m_indexWriteTarget = false;
            ResolveExpr(r, a->value, scope);

            // Whether the stored value is provably non-empty (checked before the move below empties its source).
            const char* writtenKey = MovableBoxSourceKey(r, a->target);
            const TypeName* writtenValueType = InferType(r, a->value, scope);
            bool writtenValueNonEmpty
                = writtenValueType
                  && (!writtenValueType->isOptional || IsPathNonEmpty(r, MovableBoxSourceKey(r, a->value)));

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

            // Whole fixed arrays assign by element, never as a unit (locals,
            // fields and elements alike — braced initialization at the
            // declaration is the only whole-array form).
            if (a->target->kind == NodeMember || a->target->kind == NodeIndex || a->target->kind == NodeIdent)
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

                if (vt && !IsAssignableType(r, tt, vt) && !IsStrLiteralForCString(r, tt, a->value))
                {
                    DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to '%s' of type '%s'", vt->name,
                                 ((IdentExpr*)a->target)->name, tt->name);
                }

                /* Assigning a `T?` into a plain `T` unwraps the optional. */
                CheckOptionalDeref(r, a->value, vt, tt, a->base.range);
            }
            else if (a->target->kind == NodeIndex && a->op == AssignSet)
            {
                /* Element write into an owning-element array (`Rec[]`,
                   `string[]`): the old element is dropped and the RHS moves
                   in. A borrow out of another array element would leave the
                   value owned by two arrays — same rule as push. */
                const TypeName* et = InferType(r, a->target, scope);
                const TypeName* vt = InferType(r, a->value, scope);

                if (et && AliasIsOwningValue(r, et) && !et->isBox)
                {
                    if (vt && AliasIsOwningValue(r, vt) && IsArrayElementBorrow(a->value))
                    {
                        DiagErrorFmt(r->m_diag, a->value->range,
                                     "cannot assign '%s' read from an array element - it would be owned by two "
                                     "arrays; move it into a variable first",
                                     vt->name);
                    }

                    CheckOptionalDeref(r, a->value, vt, et, a->base.range);

                    const char* movedValueKey
                        = vt && AliasIsOwningValue(r, vt) ? MovableBoxSourceKey(r, a->value) : NULL;

                    if (movedValueKey)
                    {
                        if (vt->isOptional)
                        {
                            MoveOptionalSource(r, movedValueKey, a->base.range);
                        }
                        else
                        {
                            MoveBoxIdent(r, movedValueKey, a->base.range);
                        }
                    }
                }
                else if (et && et->isBox && vt && vt->isBox)
                {
                    /* `^T` element from a `^T` value: codegen moves the box
                       pointer in and nulls the source (a variable, field or
                       another element), so record the move. */
                    const char* movedValueKey = MovableBoxSourceKey(r, a->value);

                    if (movedValueKey)
                    {
                        MoveBoxIdent(r, movedValueKey, a->base.range);
                    }
                }
            }
            else if (a->target->kind == NodeMember)
            {
                /* Non-owning field target (`bob.plain = opt;`) - the field
                    type is the unwrap target. */
                const TypeName* mt = InferType(r, a->target, scope);
                const TypeName* vt = InferType(r, a->value, scope);

                CheckOptionalDeref(r, a->value, vt, mt, a->base.range);
            }

            /* The written path holds a new value: every fact at, under, or
               aliasing it is stale. `=` re-proves a `T?` target when the
               value is provably non-empty. */
            if (writtenKey)
            {
                ClearNullableFacts(r, writtenKey);

                const TypeName* writtenType = InferType(r, a->target, scope);

                if (a->op == AssignSet && writtenType && writtenType->isOptional && writtenValueNonEmpty)
                {
                    MarkPathNonEmpty(r, writtenKey);
                }

                // Refilling a precisely-indexed element re-lives it (never an erased `a[]`).
                if (a->target->kind == NodeIndex && a->op == AssignSet && !PathKeyIsErased(writtenKey))
                {
                    ClearBoxSubtree(r, writtenKey);
                }
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
                DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s property '%s'; assign it explicitly instead",
                             inc->isDec ? "decrement" : "increment", tm->member);
                return;
            }

            if (IsFatPseudoProperty(r, InferType(r, tm->base_node, scope), tm->member))
            {
                DiagErrorFmt(r->m_diag, inc->base.range,
                             "cannot %s '.%s' (read-only; arrays grow via array_push/array_resize)",
                             inc->isDec ? "decrement" : "increment", tm->member);
                return;
            }
        }

        CheckConstAssign(r, inc->operand, inc->base.range, scope);
        ResolveExpr(r, inc->operand, scope);

        /* ++/-- on an element of constant `cstring` storage is rejected. */
        if (inc->operand->kind == NodeIndex)
        {
            Node* base = inc->operand;

            while (base && base->kind == NodeIndex)
            {
                base = ((IndexExpr*)base)->base_node;
            }

            if (base)
            {
                const TypeName* baseType = InferType(r, base, scope);

                if (baseType && TypeIsCString(&r->m_registry, baseType->name))
                {
                    DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s an element of a 'cstring' (a constant string)",
                                 inc->isDec ? "decrement" : "increment");
                    return;
                }
            }
        }

        /* ++/-- only makes sense on numeric storage; anything else would
           fall through to pointer arithmetic. Bool is not numeric: `b++`
           wraps the i1 (true -> false). */
        const TypeName* incType = InferType(r, inc->operand, scope);

        if (incType && (!IsNumeric(incType->primitiveType) || IsBoolType(&r->m_registry, incType)))
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

        bool scalarPair
            = src && (IsScalarLikeType(&r->m_registry, srcName) && IsScalarLikeType(&r->m_registry, dstName));
        bool handlePair = src && IsHandleType(&r->m_registry, srcName) && IsHandleType(&r->m_registry, dstName)
                          && (HandleExtendsFrom(&r->m_registry, dstName, srcName)
                              || HandleExtendsFrom(&r->m_registry, srcName, dstName));

        /* SIMD vector pair: same lane count (including through type aliases). */
        const int srcLanes = IsNameSimdVector(resolvedSrc);
        const int dstLanes = IsNameSimdVector(resolvedDst);
        bool simdPair = src && srcLanes != 0 && dstLanes != 0 && srcLanes == dstLanes;

        /* ^T -> ^U only when T or U is opaque (erase/cast-back). */
        bool boxPair = src && TypeNameIsOwning(src) && TypeNameIsOwning(dst)
                       && ((TypeNameBoxInner(src) && TypeRegistryIsOpaque(&r->m_registry, src->inner->name))
                           || (TypeNameBoxInner(dst) && TypeRegistryIsOpaque(&r->m_registry, dst->inner->name)));

        /* Alias <-> underlying (or alias <-> alias) of the SAME resolved type:
           the explicit escape hatch for strong typedefs (`(Name)s`, `(string)n`).
           Distinct aliases still never convert implicitly. */
        bool aliasPair
            = src && SameResolvedType(r, srcName, dstName)
              && (TypeRegistryIsTypeAlias(&r->m_registry, srcName) || TypeRegistryIsTypeAlias(&r->m_registry, dstName));

        /* `cstring` -> `string` copies (heap copy at the use site);
           a string literal casts to `cstring` (contextual `.rodata` pointer). */
        bool cstringStringPair
            = src
              && ((TypeIsCString(&r->m_registry, srcName) && TypeIsString(&r->m_registry, dstName))
                  || (cast->operand->kind == NodeStrLiteral && TypeIsCString(&r->m_registry, dstName)));

        /* Handle <-> integer: a handle reads back as its 64-bit address (`(ulong)h`, `(long)h`), and
           any integer converts to a handle (`(ptr)addr`). */
        PrimitiveType srcPrim = GetPrimitiveType(resolvedSrc);
        PrimitiveType dstPrim = GetPrimitiveType(resolvedDst);
        bool handleIntPair
            = src
              && ((IsHandleType(&r->m_registry, srcName) && (dstPrim == PrimLong || dstPrim == PrimULong))
                  || (IsHandleType(&r->m_registry, dstName) && IsNumeric(srcPrim) && !IsNameFloatType(resolvedSrc)
                      && srcPrim != PrimBool));

        /* `(cstring)p`: view raw memory as a NUL-terminated string; `(ptr)cs` the reverse. */
        bool cstringPtrPair = src
                              && ((TypeIsCString(&r->m_registry, dstName) && strcmp(resolvedSrc, STRATA_PTR_TYPE) == 0)
                                  || (TypeIsCString(&r->m_registry, srcName) && strcmp(resolvedDst, STRATA_PTR_TYPE) == 0));

        /* Text views: a NUL-terminated `byte[N]` buffer as a `cstring` (borrowed) or a `string`
           (copied, up to the first NUL or all N bytes); a `string` as a `cstring` (borrowed). */
        bool bufferSrc = src && src->isArray && src->length >= 0 && src->elem && !src->elem->isArray
                         && (strcmp(src->elem->name, "byte") == 0 || strcmp(src->elem->name, "sbyte") == 0);
        cast->textKind = CastTextNone;

        if (bufferSrc && TypeIsCString(&r->m_registry, dstName))
        {
            cast->textKind = CastBufferToCString;
        }
        else if (bufferSrc && TypeIsString(&r->m_registry, dstName))
        {
            cast->textKind = CastBufferToString;
        }
        else if (src && cast->operand->kind != NodeStrLiteral && TypeIsString(&r->m_registry, srcName)
                 && TypeIsCString(&r->m_registry, dstName))
        {
            cast->textKind = CastStringToCString;
        }
        else if (src && TypeNameIsFixedArray(src) && strcmp(resolvedDst, STRATA_PTR_TYPE) == 0)
        {
            cast->textKind = CastFixedToPtr;
        }

        if (src && !scalarPair && !handlePair && !simdPair && !boxPair && !aliasPair && !cstringStringPair
            && !handleIntPair && !cstringPtrPair && cast->textKind == CastTextNone)
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
                DiagErrorFmt(r->m_diag, m->base.range, "handle '%s' has no member '%s'", baseType->name, m->member);
            }
        }

        /* `cstring` exposes only `.length` (no `.cap`, no other members). */
        if (baseType && TypeIsCString(&r->m_registry, baseType->name) && strcmp(m->member, "length") != 0)
        {
            DiagErrorFmt(r->m_diag, m->base.range, "cstring has no member '%s' (only '.length')", m->member);
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
                    DiagPoisoned(r, m->base.range, key);
                }
                else
                {
                    DiagErrorFmt(r->m_diag, m->base.range, "'%s' used after move", key);
                }
            }
        }

        return;
    }
    case NodeConditional:
    {
        ConditionalExpr* ce = (ConditionalExpr*)n;

        ResolveExpr(r, ce->condition, scope);
        CheckBoolCondition(r, ce->condition, scope);
        ResolveUnderCondition(r, ce->thenExpr, ce->condition, true, scope);
        ResolveUnderCondition(r, ce->elseExpr, ce->condition, false, scope);
        ce->resultType = ConditionalType(r, ce, scope, true);
        return;
    }
    case NodeCall:
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

                        if (!CheckFixedArrayInitShape(r, &fieldDecl->type, ai, leaf, fieldDecl->name, "field"))
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

                            if (ai->repeatCount)
                            {
                                DiagErrorFmt(r->m_diag, field->value->range,
                                             "'{ value; count }' cannot initialize fixed-size array field '%s'; "
                                             "list the elements, or fill the field after construction",
                                             fieldDecl->name);
                                free(flat.items);
                                ai->repeatCount = NULL;
                                ai->elements.count = 0;
                                continue;
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

                            if (elemType && !IsAssignableType(r, leaf, elemType)
                                && !IsStrLiteralForCString(r, leaf, elem))
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

                            if (elemType && !IsAssignableType(r, elem, elemType)
                                && !IsStrLiteralForCString(r, elem, element))
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

                    if (fieldValueType && !IsAssignableType(r, &fieldDecl->type, fieldValueType)
                        && !IsStrLiteralForCString(r, &fieldDecl->type, field->value))
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

        /* Non-optional `^T` fields must be initialized (NULL would trip every use); `T?` may stay empty.
           Also types any nested bare-braced field values. */
        CheckStructInitFields(r, n);

        return;
    }
    case NodeIndex:
    {
        IndexExpr* ix = (IndexExpr*)n;
        bool writeTarget = r->m_indexWriteTarget;

        r->m_indexWriteTarget = false;
        ResolveExprImpl(r, ix->base_node, scope, true);
        ResolveExprImpl(r, ix->index, scope, false);

        /* An owning element moved out (`take(a[0])`) leaves a dead slot:
           reads are rejected until it is reassigned. A move through an index
           that may alias this one (`a[j]`, `a[pick()]`) counts too. */
        const TypeName* elemType = writeTarget ? NULL : InferType(r, n, scope);

        if (elemType && AliasIsOwningValue(r, elemType))
        {
            const char* key = MovableBoxSourceKey(r, n);
            const char* baseKey = MovableBoxSourceKey(r, ix->base_node);

            if (key && !(baseKey && IsBoxMoved(r, baseKey)))
            {
                if (IsPathMaybeMoved(r, key))
                {
                    DiagErrorFmt(r->m_diag, ix->base.range, "'%s' used after move", key);
                }
                else if (!asMemberBase && IsBoxPartiallyMoved(r, key))
                {
                    DiagPoisoned(r, ix->base.range, key);
                }
            }
        }

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

        if (ai->repeatCount)
        {
            ResolveArrayRepeat(r, ai, scope);
            return;
        }

        for (size_t i = 0; i < ai->elements.count; i++)
        {
            Node* elem = (Node*)VecGet(&ai->elements, i);

            // A flattened multidim fixed-array init carries NULL holes
            // (short rows / missing rows zero-fill); nothing to resolve.
            if (!elem)
            {
                continue;
            }

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

            // Owning elements move out of their sources (owning-struct
            // elements included: `{ Rec{...}, arr2[0] }` takes arr2[0]).
            if (ai->elementType && AliasIsOwningValue(r, ai->elementType))
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

/* A `defer`red statement queued on its scope. It is checked against the flow
   state at every exit it runs at (block end, return, break, continue). */
typedef struct
{
    Node* m_stmt;
    size_t m_frame;           /* index of the owning scope in m_scopeDecls */
    size_t m_declMark;        /* names the scope had declared at the `defer` */
    DiagnosticEngine m_early; /* diagnostics from the walk at the `defer` itself */
    bool m_ranAtExit;
} DeferEntry;

/* The break/continue targets of one loop iteration being walked. */
typedef struct
{
    FlowState m_breaks;    /* join of every `break`: part of the loop exit */
    FlowState m_continues; /* join of every `continue`: a back edge */
    size_t m_deferDepth;   /* m_deferFrames.count at body entry */
    int m_deferWalkDepth;  /* m_deferWalkDepth when the loop began */
} LoopFlow;

/* Upper bound on muted fixed-point passes per loop. The join is monotone
   over a finite key set so it converges long before this in practice. */
#define LOOP_MAX_PASSES 32

// Reports each diagnostic of `src` that `dst` does not hold yet.
static void ForwardNewDiagnostics(DiagnosticEngine* dst, const DiagnosticEngine* src)
{
    for (size_t i = 0; i < src->m_count; i++)
    {
        const Diagnostic* d = &src->m_diagnostics[i];
        bool seen = false;

        for (size_t j = 0; j < dst->m_count && !seen; j++)
        {
            const Diagnostic* e = &dst->m_diagnostics[j];

            seen = e->severity == d->severity && e->range.start == d->range.start && e->range.length == d->range.length
                   && e->range.fileId == d->range.fileId && strcmp(e->message, d->message) == 0;
        }

        if (!seen)
        {
            DiagReport(dst, d->severity, d->range, d->message);
        }
    }
}

/* Walks a deferred statement at one of its exits. Names declared after the
   `defer` are hidden, and repeats of a diagnostic from another exit are
   dropped. */
static void WalkDeferred(Resolver* r, DeferEntry* e, StrMap* scope)
{
    Vec hidden;
    VecInit(&hidden);

    for (size_t f = e->m_frame; f < r->m_scopeDecls.count; f++)
    {
        Vec* frame = (Vec*)VecGet(&r->m_scopeDecls, f);

        for (size_t k = f == e->m_frame ? e->m_declMark : 0; k < frame->count; k++)
        {
            const char* name = (const char*)VecGet(frame, k);

            VecPush(&hidden, (void*)name);
            VecPush(&hidden, StrMapGet(scope, name));
            StrMapPut(scope, name, NULL);
        }
    }

    DiagnosticEngine scratch;
    DiagnosticEngineInit(&scratch);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &scratch;

    bool wasUnreachable = r->m_unreachable;
    r->m_deferWalkDepth++;

    WalkStmt(r, e->m_stmt, scope);

    r->m_deferWalkDepth--;
    r->m_unreachable = wasUnreachable;
    r->m_diag = realDiag;

    ForwardNewDiagnostics(r->m_diag, &scratch);
    DiagnosticEngineFree(&scratch);

    for (size_t i = hidden.count; i >= 2; i -= 2)
    {
        StrMapPut(scope, (const char*)VecGet(&hidden, i - 2), VecGet(&hidden, i - 1));
    }

    free(hidden.items);

    e->m_ranAtExit = true;
}

// Runs one scope's deferred statements, last `defer` first.
static void RunDeferFrame(Resolver* r, Vec* frame, StrMap* scope)
{
    for (size_t i = frame->count; i-- > 0;)
    {
        WalkDeferred(r, (DeferEntry*)VecGet(frame, i), scope);
    }
}

// Runs the deferred statements of scopes [fromIndex, top), innermost first.
static void RunDefersFrom(Resolver* r, size_t fromIndex, StrMap* scope)
{
    for (size_t f = r->m_deferFrames.count; f-- > fromIndex;)
    {
        RunDeferFrame(r, (Vec*)VecGet(&r->m_deferFrames, f), scope);
    }
}

// Records a `defer` on the innermost scope. Its checks happen at the exits.
static void QueueDefer(Resolver* r, DeferStmt* d, StrMap* scope)
{
    if (r->m_deferFrames.count == 0 || r->m_scopeDecls.count == 0)
    {
        WalkStmt(r, d->stmt, scope);
        return;
    }

    DeferEntry* e = (DeferEntry*)arena_alloc(r->m_arena, sizeof(DeferEntry));
    e->m_stmt = d->stmt;
    e->m_frame = r->m_scopeDecls.count - 1;
    e->m_declMark = ((Vec*)VecGet(&r->m_scopeDecls, e->m_frame))->count;
    e->m_ranAtExit = false;

    /* Resolve it here, in the scope it was written in, but keep the flow
       state untouched: nothing in it has run yet. The diagnostics are only
       used if no reachable exit ever runs the statement. */
    FlowState saved;
    FlowInit(&saved);
    FlowCapture(r, &saved);

    DiagnosticEngineInit(&e->m_early);
    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &e->m_early;
    r->m_deferWalkDepth++;

    WalkStmt(r, d->stmt, scope);

    r->m_deferWalkDepth--;
    r->m_diag = realDiag;

    ReplaceStrMapContents(&r->m_movedBoxes, &saved.m_moved);
    ReplaceStrMapContents(&r->m_nonEmptyPaths, &saved.m_nonEmpty);
    ReplaceStrMapContents(&r->m_emptyPaths, &saved.m_empty);
    r->m_unreachable = !saved.m_live;
    FlowFree(&saved);

    VecPush((Vec*)VecGet(&r->m_deferFrames, r->m_deferFrames.count - 1), e);
}

/* `break`/`continue`: run the defers of the scopes being left, then hand the
   state to the loop's exit or back edge. The path after it is dead. */
static void WalkLoopJump(Resolver* r, StrMap* scope, bool isBreak)
{
    LoopFlow* lf = r->m_loopFlows.count > 0 ? (LoopFlow*)VecGet(&r->m_loopFlows, r->m_loopFlows.count - 1) : NULL;

    /* A jump out of a deferred statement is not modeled; it only ends the path. */
    if (lf && lf->m_deferWalkDepth == r->m_deferWalkDepth && !r->m_unreachable)
    {
        RunDefersFrom(r, lf->m_deferDepth, scope);
        FlowJoinCurrent(r, isBreak ? &lf->m_breaks : &lf->m_continues);
    }

    r->m_unreachable = true;
}

/* `if`/`while`/`for` conditions must be `bool`: no implicit truthiness for
   numerics (`if (x)` on a float is a != 0.0 test, rarely what was meant). */
static void CheckBoolCondition(Resolver* r, Node* cond, StrMap* scope)
{
    const TypeName* t = cond ? InferType(r, cond, scope) : NULL;

    if (t && !IsBoolContextType(&r->m_registry, t))
    {
        DiagErrorFmt(r->m_diag, cond->range, "condition must be of type 'bool', found '%s'", t->name);
    }
}

/* One trip around the loop from the current (loop head) state: condition,
   body, continue join, update. `exit` receives the condition-false and break
   states, `back` the state flowing back to the head. */
static void WalkLoopIteration(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope,
                              FlowState* exit, FlowState* back)
{
    LoopFlow lf;
    FlowInit(&lf.m_breaks);
    FlowInit(&lf.m_continues);
    lf.m_deferDepth = r->m_deferFrames.count;
    lf.m_deferWalkDepth = r->m_deferWalkDepth;

    if (condition)
    {
        ResolveExpr(r, condition, scope);
        CheckBoolCondition(r, condition, scope);
    }

    // Condition false: leave the loop with the opposite facts.
    if (condition && !ExprIsConstantTrue(condition))
    {
        FlowState afterCond;
        FlowInit(&afterCond);
        FlowCapture(r, &afterCond);

        ApplyCondFacts(r, condition, false, false, true);

        FlowJoinCurrent(r, exit);
        FlowApply(r, &afterCond);
        FlowFree(&afterCond);
    }

    // Condition true: its facts hold through the body.
    if (condition)
    {
        ApplyCondFacts(r, condition, true, true, false);
    }

    VecPush(&r->m_loopFlows, &lf);

    if (bodyScope)
    {
        ScopePush(r);
    }

    WalkStmt(r, body, scope);

    if (bodyScope)
    {
        ScopePop(r, scope);
    }

    VecPop(&r->m_loopFlows);

    // Falling off the body end and every `continue` meet before the update.
    FlowJoinCurrent(r, &lf.m_continues);
    FlowApply(r, &lf.m_continues);

    if (update)
    {
        ResolveExpr(r, update, scope);
    }

    FlowCapture(r, back);
    FlowJoin(exit, &lf.m_breaks);

    FlowFree(&lf.m_breaks);
    FlowFree(&lf.m_continues);
}

/* Loops iterate muted until the head state (entry joined with every back
   edge) is stable, then walk once more from that state for diagnostics. The
   state after the loop joins the condition-false exit with every `break`. */
static void WalkLoop(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope)
{
    FlowState head;
    FlowInit(&head);
    FlowCapture(r, &head);

    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    for (int pass = 0; pass < LOOP_MAX_PASSES; pass++)
    {
        FlowState exit;
        FlowState back;
        FlowInit(&exit);
        FlowInit(&back);

        FlowApply(r, &head);
        WalkLoopIteration(r, condition, update, body, bodyScope, scope, &exit, &back);

        FlowState next;
        FlowInit(&next);
        FlowJoin(&next, &head);
        FlowJoin(&next, &back);

        bool stable = FlowSame(&next, &head);

        FlowFree(&head);
        head = next;

        FlowFree(&exit);
        FlowFree(&back);

        if (stable)
        {
            break;
        }
    }

    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    FlowState exit;
    FlowState back;
    FlowInit(&exit);
    FlowInit(&back);

    FlowApply(r, &head);
    WalkLoopIteration(r, condition, update, body, bodyScope, scope, &exit, &back);
    FlowApply(r, &exit);

    FlowFree(&head);
    FlowFree(&exit);
    FlowFree(&back);
}

/**
 * @brief Pushes a new scope to the scopeDecls stack.
 */
static void ScopePush(Resolver* r)
{
    Vec* frame = (Vec*)arena_alloc(r->m_arena, sizeof(Vec));
    VecInit(frame);
    VecPush(&r->m_scopeDecls, frame);

    Vec* defers = (Vec*)arena_alloc(r->m_arena, sizeof(Vec));
    VecInit(defers);
    VecPush(&r->m_deferFrames, defers);
}

/**
 * @brief Runs the scope's defers (when its end is reachable) and invalidates all symbols in it.
 */
static void ScopePop(Resolver* r, StrMap* scope)
{
    if (r->m_deferFrames.count > 0)
    {
        Vec* defers = (Vec*)VecPop(&r->m_deferFrames);

        if (!r->m_unreachable)
        {
            RunDeferFrame(r, defers, scope);
        }

        for (size_t i = 0; i < defers->count; i++)
        {
            DeferEntry* e = (DeferEntry*)VecGet(defers, i);

            /* No reachable exit ran it: keep what the `defer` site found. */
            if (!e->m_ranAtExit)
            {
                ForwardNewDiagnostics(r->m_diag, &e->m_early);
            }

            DiagnosticEngineFree(&e->m_early);
        }

        free(defers->items);
    }

    if (r->m_scopeDecls.count == 0)
    {
        return;
    }

    Vec* frame = (Vec*)VecPop(&r->m_scopeDecls);

    for (size_t i = 0; i < frame->count; i++)
    {
        const char* name = (const char*)VecGet(frame, i);
        StrMapPut(scope, name, NULL);
        ClearBoxSubtree(r, name);
        ClearNullableFacts(r, name);
        InvalidateIndexVar(r, name);
    }
}

/**
 * @brief Declares a new symbol `name` in the scope `scope`.
 */
static void ScopeDeclare(Resolver* r, StrMap* scope, const char* name, void* typePtr, SourceRange range)
{
    void* existing = StrMapGet(scope, name);

    if (existing && existing != typePtr)
    {
        DiagErrorFmt(r->m_diag, range, "redefinition of '%s'", name);
    }

    StrMapPut(scope, name, typePtr);

    if (r->m_scopeDecls.count > 0)
    {
        Vec* top = (Vec*)VecGet(&r->m_scopeDecls, r->m_scopeDecls.count - 1);
        VecPush(top, (void*)name);
    }
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

        if (vd->isRef)
        {
            ResolveRefLocal(r, vd, scope);
            return;
        }

        /* A plain local shadows any earlier `ref` local of the same name. */
        if (StrMapGet(&r->m_refLocals, vd->name))
        {
            StrMapPut(&r->m_refLocals, vd->name, NULL);
            StrMapPut(&r->m_refThroughBox, vd->name, NULL);
            StrMapPut(&r->m_refBoxParams, vd->name, NULL);
        }

        // Unknown types must fail here so codegen is skipped.
        {
            const TypeName* t = &vd->type;
            while (t && (t->isBox || t->isOptional || t->isArray))
            {
                t = (t->isBox || t->isOptional) ? t->inner : t->elem;
            }
            if (t && t->name && !IsStringType(t->primitiveType) && !IsCStringType(t->primitiveType)
                && !IsScalarType(t->primitiveType) && !IsSimdVector(t->primitiveType)
                && !TypeRegistryIsUserType(&r->m_registry, t->name))
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "unknown type '%s'", t->name);
                return;
            }
        }

        if (IsIncompleteStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, vd->type.name);
        }

        /* Resolve `[constName]` dimensions before the fixed-array checks below
           (idempotent: lengthName is cleared once resolved, so loop warmups
           re-resolving the same declaration are harmless). */
        SemaResolveConstDims(r, &vd->type);

        /* Stack-allocated fixed-size array locals (`float v[10] = { ... };`):
           inline `[N x T]` storage, braced initialization only. Params,
           returns and globals keep their bans (checked after all functions). */
        if (TypeTreeHasFixedArray(&vd->type))
        {
            if (!(vd->type.isArray && vd->type.length >= 0))
            {
                /* e.g. `int[][4] x;` — a dynamic array of fixed arrays. */
                DiagErrorFmt(r->m_diag, vd->base.range,
                             "local variable '%s' has invalid fixed-size array type ('%s'); "
                             "fixed-size arrays may only be declared as 'T[N]' locals",
                             vd->name, vd->type.name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            if (vd->type.length < 1)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "fixed-size array local '%s' must have a length of at least 1",
                             vd->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            long total = 0;
            const TypeName* leaf = FixedArrayLeaf(&vd->type, &total);

            if (leaf->isArray)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "fixed-size array local '%s' may not contain a dynamic array",
                             vd->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            if (leaf->isBox || AliasIsOwning(r, leaf))
            {
                DiagErrorFmt(r->m_diag, vd->base.range,
                             "fixed-size array local '%s' may not own its elements ('%s' is owning); "
                             "fixed-size arrays have no drop glue",
                             vd->name, leaf->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            if (TypeRegistryIsOwningStruct(&r->m_registry, leaf->name))
            {
                DiagErrorFmt(r->m_diag, vd->base.range,
                             "fixed-size array local '%s' may not contain an owning struct ('%s'); "
                             "fixed-size arrays have no drop glue",
                             vd->name, leaf->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            if (IsIncompleteStruct(&r->m_registry, leaf->name))
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, leaf->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            if (!vd->init || vd->init->kind != NodeArrayInit)
            {
                DiagErrorFmt(r->m_diag, vd->base.range,
                             "fixed-size array local '%s' must be initialized with a braced list "
                             "('{ ... }'), e.g. 'float %s[%ld] = { ... }'",
                             vd->name, vd->name, vd->type.length);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            ArrayInitExpr* fixedInit = (ArrayInitExpr*)vd->init;

            /* `{ value; N }`: every element (of every row) is the value; N must be the full length. */
            if (fixedInit->repeatCount)
            {
                fixedInit->elementType = leaf;
                ResolveExpr(r, fixedInit->repeatCount, scope);

                uint64_t repeat = 0;
                bool negative = false;

                if (!ResolvedConstIntValue(fixedInit->repeatCount, &repeat, &negative) || negative
                    || repeat != (uint64_t)total)
                {
                    DiagErrorFmt(r->m_diag, fixedInit->repeatCount->range,
                                 "the count in '{ value; count }' for fixed-size array local '%s' must be the "
                                 "constant %ld (its length)",
                                 vd->name, total);
                }

                Node* value = (Node*)VecGet(&fixedInit->elements, 0);

                if (value->kind == NodeArrayInit || value->kind == NodeStructInit)
                {
                    value = ApplyBracedStructTarget(r, value, leaf);
                    VecSet(&fixedInit->elements, 0, value);
                }

                ResolveExpr(r, value, scope);

                const TypeName* valueType = InferType(r, value, scope);

                if (valueType && !IsAssignableType(r, leaf, valueType) && !IsStrLiteralForCString(r, leaf, value))
                {
                    DiagErrorFmt(r->m_diag, value->range, "a '%s' cannot fill the '%s' elements of local '%s'",
                                 valueType->name, leaf->name, vd->name);
                }

                ClearBoxSubtree(r, vd->name);
                ClearNullableFacts(r, vd->name);
                InvalidateIndexVar(r, vd->name);
                ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                return;
            }

            /* Loop bodies walk twice (muted warmup + real); a multidim init
               flattened by the first pass must not fail the shape check on
               the second. An already-flat init is the output of this very
               processing — detect it BEFORE stamping elementType below (a
               user-written flat multidim init still carries the parser's
               intermediate element type, so it is correctly NOT skipped and
               fails the shape check). Error paths never mutate the init, so
               re-walks reproduce their diagnostics loudly instead of
               converging silent. */
            bool alreadyFlat = fixedInit->elementType == leaf && fixedInit->elements.count == (size_t)total
                               && !HasNestedArrayInit(fixedInit);

            if (!alreadyFlat)
            {
                if (!CheckFixedArrayInitShape(r, &vd->type, fixedInit, leaf, vd->name, "local"))
                {
                    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                    return;
                }

                /* The parser pre-fills elementType with the immediate element
                   (`int[2]` for `int[2][2]`); codegen needs the LEAF (`int`),
                   like struct-field inits (which parse with NULL and get the
                   leaf here). */
                fixedInit->elementType = leaf;

                /* Short rows leave holes that zero-fill. */
                Vec flat;
                VecInit(&flat);

                for (long z = 0; z < total; z++)
                {
                    VecPush(&flat, NULL);
                }

                if (!PlaceFixedArrayInit(&flat, 0, &vd->type, fixedInit))
                {
                    DiagErrorFmt(r->m_diag, vd->init->range,
                                 "too many initializers for fixed-size array local '%s' (%ld max)", vd->name, total);
                    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
                    return;
                }

                void** oldItems = fixedInit->elements.items;
                fixedInit->elements = flat;
                free(oldItems);
            }

            /* Bare-brace leaves are struct inits of the leaf type. */
            for (size_t k = 0; k < fixedInit->elements.count; k++)
            {
                Node* elem = (Node*)VecGet(&fixedInit->elements, k);

                if (!elem)
                {
                    continue;
                }

                if (elem->kind == NodeArrayInit || elem->kind == NodeStructInit)
                {
                    VecSet(&fixedInit->elements, k, ApplyBracedStructTarget(r, elem, leaf));
                }
            }

            ResolveExpr(r, vd->init, scope);

            for (size_t k = 0; k < fixedInit->elements.count; k++)
            {
                Node* elem = (Node*)VecGet(&fixedInit->elements, k);

                if (!elem)
                {
                    continue; // Hole: stays zero.
                }

                const TypeName* elemType = InferType(r, elem, scope);

                if (elemType && !IsAssignableType(r, leaf, elemType) && !IsStrLiteralForCString(r, leaf, elem))
                {
                    DiagErrorFmt(r->m_diag, elem->range,
                                 "element of type '%s' cannot initialize '%s' element of local '%s'", elemType->name,
                                 leaf->name, vd->name);
                }
            }

            ClearBoxSubtree(r, vd->name);
            ClearNullableFacts(r, vd->name);
            InvalidateIndexVar(r, vd->name);
            ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
            return;
        }

        // Owning locals need an init (arrays/optionals exempt; a bare owning
        // struct local is rejected below with a sharper message).
        if (AliasIsOwning(r, &vd->type) && !TypeNameIsDynamicArray(&vd->type) && !TypeNameIsOptional(&vd->type)
            && !TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name) && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "box variable '%s' must be initialized", vd->name);
        }

        if (vd->type.isConst && !vd->init)
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "const variable '%s' must be initialized", vd->name);
        }

        if (TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "instances of '%s' must be boxed (i.e, '^%s').", vd->type.name,
                         vd->type.name);
        }

        // Dynamic arrays of owning structs (`Rec[]`) are legal: the fat owns
        // the buffer and drops each inline element recursively, just like
        // `string[]`. Fixed-array locals return early above (they are
        // non-owning by construction), so nothing here sees one.

        bool initProvesNonEmpty = true;

        if (vd->init)
        {
            ResolveExpr(r, vd->init, scope);
            const TypeName* initType = InferType(r, vd->init, scope);

            bool ok = initType
                      && (IsAssignableType(r, &vd->type, initType) || IsStrLiteralForCString(r, &vd->type, vd->init));

            if (initType && !ok)
            {
                DiagErrorFmt(r->m_diag, vd->base.range, "'%s' cannot be initialized by expression of type '%s'",
                             vd->type.name, initType->name);
            }

            // Plain `T` from `T?` unwraps.
            CheckOptionalDeref(r, vd->init, initType, &vd->type, vd->base.range);

            // Box init from a box source moves it; a `Rec` source (owning
            // struct value, e.g. an element of a `Rec[]`) moves too.
            const char* movedInitKey = initType && AliasIsOwning(r, &vd->type) && AliasIsOwningValue(r, initType)
                                           ? MovableBoxSourceKey(r, vd->init)
                                           : NULL;

            // A `T?` init proves non-empty only when itself proven.
            if (vd->type.isOptional)
            {
                initProvesNonEmpty
                    = (initType && !initType->isOptional) || (movedInitKey && IsPathNonEmpty(r, movedInitKey));
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

        // Fresh binding: drop stale flow state for this name.
        ClearBoxSubtree(r, vd->name);
        ClearNullableFacts(r, vd->name);
        InvalidateIndexVar(r, vd->name);

        // An init proves non-empty unless it is a maybe-empty `T?`.
        if (vd->init && initProvesNonEmpty)
        {
            MarkPathNonEmpty(r, vd->name);
        }

        ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);

        return;
    }
    case NodeExprStmt:
        ResolveExpr(r, ((ExprStmt*)n)->expr, scope);
        /* A call that never returns ends the path, as `return` does. */
        if (StmtIsNoReturnCall(n))
        {
            r->m_unreachable = true;
        }
        return;
    case NodeDefer:
    {
        // Runs (and is checked) at each exit of the enclosing scope.
        QueueDefer(r, (DeferStmt*)n, scope);
        return;
    }
    case NodeBreak:
        WalkLoopJump(r, scope, true);
        return;
    case NodeContinue:
        WalkLoopJump(r, scope, false);
        return;
    case NodeReturn:
    {
        ReturnStmt* rs = (ReturnStmt*)n;
        if (rs->value)
        {
            if (r->m_currentReturnType && r->m_currentReturnType->primitiveType == PrimVoid)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "void function cannot return a value");
            }

            ResolveExpr(r, rs->value, scope);

            const TypeName* typeName = InferType(r, rs->value, scope);

            if (typeName && typeName->primitiveType == PrimVoid)
            {
                DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type 'void'");
            }

            // Check the returned value against the declared return type.
            if (r->m_currentReturnType && r->m_currentReturnType->primitiveType != PrimVoid && typeName
                && !IsAssignableType(r, r->m_currentReturnType, typeName)
                && !IsStrLiteralForCString(r, r->m_currentReturnType, rs->value))
            {
                DiagErrorFmt(r->m_diag, rs->base.range,
                             "cannot return a value of type '%s' from a function returning '%s'", typeName->name,
                             r->m_currentReturnType->name);
            }

            // Returning `T?` from `T` unwraps.
            CheckOptionalDeref(r, rs->value, typeName, r->m_currentReturnType, rs->base.range);

            // Moves only when the function returns an owning type (boxes,
            // strings, arrays, and owning structs — `return arr[i];` from a
            // `Rec`-returning function empties the element).
            const char* movedReturnKey
                = typeName && AliasIsOwningValue(r, typeName) ? MovableBoxSourceKey(r, rs->value) : NULL;

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
                                  || (IsNameNumeric(boxInnerName) && IsNameNumeric(r->m_currentReturnType->name))));

                    if (innerIsOwning || !innerMatchesReturn)
                    {
                        MoveBoxIdent(r, movedReturnKey, rs->base.range);
                    }
                }
            }
        }
        else if (r->m_currentReturnType && r->m_currentReturnType->primitiveType != PrimVoid)
        {
            DiagErrorFmt(r->m_diag, rs->base.range, "non-void function must return a value");
        }

        // Every pending defer runs after the value is computed.
        if (r->m_deferWalkDepth == 0 && !r->m_unreachable)
        {
            RunDefersFrom(r, 0, scope);
        }

        r->m_unreachable = true;

        return;
    }
    case NodeIf:
    {
        IfStmt* i = (IfStmt*)n;
        ResolveExpr(r, i->condition, scope);
        CheckBoolCondition(r, i->condition, scope);

        /* `if (path?)` blesses the then-branch; `if (!path?)` blesses the else; `&&` and `||` of such
         * tests bless each on the side where they all hold. Facts intersect at the join; moved-state
         * unions. */

        /* then/else are walked from the same pre-state; the result merges both. */
        StrMap beforeBranches;
        CopyStrMap(&r->m_movedBoxes, &beforeBranches);

        StrMap beforeFacts;
        CopyStrMap(&r->m_nonEmptyPaths, &beforeFacts);

        StrMap beforeEmpty;
        CopyStrMap(&r->m_emptyPaths, &beforeEmpty);

        bool entryDead = r->m_unreachable;

        ApplyCondFacts(r, i->condition, true, true, false);

        /* Each branch is its own lexical scope: locals declared in one
           branch are invisible in the other and after the join. */
        ScopePush(r);
        WalkStmt(r, i->thenBranch, scope);
        ScopePop(r, scope);

        bool thenDead = r->m_unreachable;
        r->m_unreachable = entryDead;

        StrMap afterThen;
        CopyStrMap(&r->m_movedBoxes, &afterThen);

        StrMap factsThen;
        CopyStrMap(&r->m_nonEmptyPaths, &factsThen);

        ReplaceStrMapContents(&r->m_movedBoxes, &beforeBranches);
        ReplaceStrMapContents(&r->m_nonEmptyPaths, &beforeFacts);
        /* Empty facts never escape their branch. */
        ReplaceStrMapContents(&r->m_emptyPaths, &beforeEmpty);

        /* The implicit/explicit else runs when the condition is false: establishes the opposite facts
           (empty ones only inside an explicit else; they never escape it). */
        ApplyCondFacts(r, i->condition, false, false, false);

        if (i->elseBranch)
        {
            ApplyCondFacts(r, i->condition, false, true, false);

            ScopePush(r);
            WalkStmt(r, i->elseBranch, scope);
            ScopePop(r, scope);

            ReplaceStrMapContents(&r->m_emptyPaths, &beforeEmpty);
        }

        bool elseDead = r->m_unreachable;

        /* A branch that never reaches the join (return, break, continue) must
         * not poison the fall-through path: break/continue states were
         * already handed to their loop. An else-less if falls through on its
         * false edge. Both arms dead leaves the join unreachable - any state
         * is sound; the then-side is adopted. */
        bool elseTerminates = elseDead && !entryDead;
        bool thenTerminates = thenDead && !entryDead;

        r->m_unreachable = thenDead && elseDead;

        if (elseTerminates)
        {
            /* Only the then-path reaches the join: adopt its state. */
            ReplaceStrMapContents(&r->m_movedBoxes, &afterThen);
            ReplaceStrMapContents(&r->m_nonEmptyPaths, &factsThen);
        }
        else if (!thenTerminates)
        {
            MergeMovedBoxes(&r->m_movedBoxes, &afterThen);

            /* Keep only paths proven non-empty on BOTH branches. */
            MergeNonEmptyFacts(&r->m_nonEmptyPaths, &factsThen);
        }

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

        /* The loop body is its own scope (covers non-block bodies too;
           block bodies nest a second scope harmlessly). */
        WalkLoop(r, w->condition, NULL, w->body, true, scope);

        return;
    }
    case NodeFor:
    {
        ForStmt* fs = (ForStmt*)n;

        /* The whole loop (init, condition, update, body) is one scope so a
           `for (int i ...)` header variable is visible in the body but does
           not leak after the loop. */
        ScopePush(r);

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

        // The condition and update run on every iteration, not just once.
        WalkLoop(r, fs->condition, fs->update, fs->body, false, scope);

        ScopePop(r, scope);

        return;
    }
    default:
        return;
    }
}

static void WalkBlock(Resolver* r, Block* b, StrMap* scope)
{
    ScopePush(r);

    for (size_t i = 0; i < b->statements.count; i++)
    {
        WalkStmt(r, (Node*)VecGet(&b->statements, i), scope);
    }

    ScopePop(r, scope);
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

    if (StmtIsNoReturnCall(n))
    {
        return false;
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

    if (type->isBox)
    {
        return "an owning box";
    }

    if (type->isOptional)
    {
        return "an optional";
    }

    if (type->isArray)
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
    const char* leaf = (type->isArray || type->isBox || type->isOptional)
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
        DiagErrorFmt(r->m_diag, field->defaultValue->range, "default value for field '%s' of type '%s' can't be of type '%s'",
                     field->name, type->name, valueType->name);
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
            folded.intValue = value.isFloat ? (long long)value.f : value.i;
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

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena)
{
    Resolver r = {0};
    r.m_mod = mod;
    r.m_diag = diag;
    r.m_arena = arena;

    MergeExternRedeclarations(mod, diag);
    MarkGrowableArrayParams(mod);
    BindDropMethods(mod, diag, arena);

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

    /* A union's fields overlay one another, so none may own memory (nothing could tell which to drop). */
    for (size_t i = 0; i < mod->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet(&mod->structs, i);

        for (size_t j = 0; sd->isUnion && j < sd->fields.count; j++)
        {
            FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);
            const char* leaf = TypeRegistryResolveAlias(&r.m_registry, field->type.name);

            if (AliasIsOwning(&r, &field->type) || TypeRegistryIsOwningStruct(&r.m_registry, leaf))
            {
                DiagErrorFmt(diag, field->nameRange, "union field '%s' cannot have owning type '%s'", field->name,
                             field->type.name);
            }
        }
    }

    StrMapInit(&r.m_constVars);
    StrMapInit(&r.m_movedBoxes);
    StrMapInit(&r.m_nonEmptyPaths);
    StrMapInit(&r.m_emptyPaths);
    StrMapInit(&r.m_indexDeps);
    StrMapInit(&r.m_boxGlobals);
    StrMapInit(&r.m_refBoxParams);
    StrMapInit(&r.m_refLocals);
    StrMapInit(&r.m_refThroughBox);
    StrMapInit(&r.m_refArrayParams);
    StrMapInit(&r.m_typeCache);
    VecInit(&r.m_scopeDecls);
    VecInit(&r.m_deferFrames);
    VecInit(&r.m_loopFlows);

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

        if (overloaded && (functionDecl->isExtern || functionDecl->isExport))
        {
            DiagErrorFmt(diag, functionDecl->base.range, "%s function '%s' cannot be overloaded",
                         functionDecl->isExtern ? "extern" : "'@export'", functionDecl->name);
        }

        if (functionDecl->isExport)
        {
            CheckExportDecl(&r, functionDecl);
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

        if (functionDecl->typeParam)
        {
            CheckGenericExternDecl(&r, functionDecl);
        }
    }

    for (size_t i = 0; i < mod->delegates.count; i++)
    {
        CheckDelegateDecl(&r, (FunctionDecl*)VecGet(&mod->delegates, i));
    }

    InferBorrowedParams(&r);

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

        // All flow state is per function: nothing proven in one holds in the next.
        ResetStrMap(&r.m_constVars);
        ResetStrMap(&r.m_movedBoxes);
        ResetStrMap(&r.m_nonEmptyPaths);
        ResetStrMap(&r.m_emptyPaths);
        ResetStrMap(&r.m_indexDeps);
        ResetStrMap(&r.m_refBoxParams);
        ResetStrMap(&r.m_refLocals);
        ResetStrMap(&r.m_refThroughBox);
        ResetStrMap(&r.m_refArrayParams);
        r.m_scopeDecls.count = 0;
        r.m_deferFrames.count = 0;
        r.m_loopFlows.count = 0;
        r.m_deferWalkDepth = 0;
        r.m_unreachable = false;

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

            if (StrMapGet(&scope, p->name))
            {
                DiagErrorFmt(diag, p->base.range, "redefinition of '%s'", p->name);
            }

            StrMapPut(&scope, p->name, (void*)&p->type);

            if (p->type.isConst)
            {
                StrMapPut(&r.m_constVars, p->name, (void*)1);
            }

            if (AliasIsOwning(&r, &p->type) && p->mod == ModRef)
            {
                StrMapPut(&r.m_refBoxParams, p->name, (void*)1);
            }

            /* A `ref T[]` binding may be a borrowed stack view (fixed-array
               slice): rebinds and growing writes would free caller storage. */
            if (p->mod == ModRef && TypeNameIsDynamicArray(&p->type))
            {
                StrMapPut(&r.m_refArrayParams, p->name, (void*)1);
            }
        }

        r.m_currentReturnType = &functionDecl->returnType;
        r.m_currentFunction = functionDecl;

        WalkBlock(&r, (Block*)functionDecl->body, &scope);
        r.m_currentReturnType = NULL;
        r.m_currentFunction = NULL;

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

        StrMapFree(&scope);
    }

    {
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
                CheckComponentFields(&r, sd);
            }

            for (size_t j = 0; j < sd->fields.count; j++)
            {
                FieldDecl* field = (FieldDecl*)VecGet(&sd->fields, j);

                if (field->defaultValue)
                {
                    ResolveFieldDefault(&r, field, &defaultScope);
                }
            }
        }

        StrMapFree(&defaultScope);
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

            if (sd->isExtern
                && (TypeIsString(&r.m_registry, field->type.name) || ResolvesToDynamicArray(&r, &field->type)))
            {
                // Strings and dynamic arrays are fats; extern structs must match C layout.
                if (TypeIsString(&r.m_registry, field->type.name))
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

                    if (initType && !IsAssignableType(&r, &gd->type, initType)
                        && !IsStrLiteralForCString(&r, &gd->type, gd->init))
                    {
                        DiagErrorFmt(diag, gd->base.range,
                                     "global '%s' of type '%s' cannot be initialized by expression of type '%s'",
                                     gd->name, gd->type.name, initType->name);
                    }
                }

                continue;
            }

            // String globals default to empty, like arrays.
            bool stringLike
                = TypeIsString(&r.m_registry, gd->type.name)
                  || (gd->type.isOptional && gd->type.inner && TypeIsString(&r.m_registry, gd->type.inner->name));

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
                             "fixed-size arrays are only allowed as struct fields and locals",
                             p->name, p->type.name);
            }

            /* By value, an owning struct argument is deep-copied for the callee: never for a destructor. */
            if (!functionDecl->isExtern && p->mod == ModNone && !p->type.isBox && !p->type.isOptional
                && !p->type.isArray && TypeNameHasDrop(&r.m_registry, &p->type))
            {
                DiagErrorFmt(diag, p->base.range,
                             "parameter '%s' cannot take '%s' by value: it has a Drop method; pass 'ref %s' or a box '^%s'",
                             p->name, p->type.name, p->type.name, p->type.name);
            }

            /* Structs (forward-declared ones included) cross to the host as pointers, so the
               declaration has to say so. */
            const char* paramLeaf = TypeRegistryResolveAlias(&r.m_registry, p->type.name);

            if (functionDecl->isExtern && p->mod == ModNone && !p->type.isBox && !p->type.isOptional
                && !TypeNameIsDynamicArray(&p->type)
                && (IsDefinedStruct(&r.m_registry, paramLeaf) || IsIncompleteStruct(&r.m_registry, paramLeaf)))
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
            && IsDefinedStruct(&r.m_registry, TypeRegistryResolveAlias(&r.m_registry, functionDecl->returnType.name)))
        {
            DiagError(diag, functionDecl->base.range, "extern function cannot return a struct type by value");
        }

        /* Fat-pointer type returns are disallowed for externs. A bare `string`
           return IS allowed: it crosses as a NUL-terminated char* that the
           caller copies into a fresh owned buffer (codegen BuildOwnedStringFromCStr). */
        if (functionDecl->isExtern && !functionDecl->hasReturnParam)
        {
            const char* leaf = TypeRegistryResolveAlias(&r.m_registry, functionDecl->returnType.name);
            TypeName parsed = TypeNameParse(r.m_arena, leaf);

            const TypeName* unwrapped = parsed.isOptional ? parsed.inner : &parsed;
            bool isFat = (unwrapped && TypeNameIsDynamicArray(unwrapped))
                         || (parsed.isOptional && TypeIsString(&r.m_registry, unwrapped ? unwrapped->name : NULL));

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
    StrMapFree(&r.m_emptyPaths);
    StrMapFree(&r.m_indexDeps);
    StrMapFree(&r.m_boxGlobals);
    StrMapFree(&r.m_refBoxParams);
    StrMapFree(&r.m_refLocals);
    StrMapFree(&r.m_refThroughBox);
    StrMapFree(&r.m_refArrayParams);
    StrMapFree(&r.m_typeCache);
    StrMapFree(&byMangled);
    free(r.m_deferFrames.items);
    free(r.m_loopFlows.items);
    TypeRegistryFree(&r.m_registry);
}
