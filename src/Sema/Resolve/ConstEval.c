// Compile-time constant evaluation, literal range checks and lossless-conversion checks.
#include "Sema/Resolve/ResolverInternal.h"

#include <inttypes.h>

// The runtime integer type of a scalar. Anything that is not an integer carries no narrowing.
ConstIntType SemaConstIntType(PrimitiveType p)
{
    switch (p)
    {
    case PrimSByte:
        return (ConstIntType){8, false};
    case PrimByte:
        return (ConstIntType){8, true};
    case PrimShort:
        return (ConstIntType){16, false};
    case PrimUShort:
        return (ConstIntType){16, true};
    case PrimInt:
        return (ConstIntType){32, false};
    case PrimUInt:
        return (ConstIntType){32, true};
    case PrimLong:
        return (ConstIntType){64, false};
    case PrimULong:
        return (ConstIntType){64, true};
    default:
        return (ConstIntType){0, false};
    }
}

// The scalar integral types an enum may be based on.
bool IsEnumUnderlyingType(const char* t)
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

// Signed bounds for the enum underlying types.
void EnumSignedRange(const char* underlying, bool* isUnsigned, ConstInt* minVal, ConstUInt* maxVal)
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
        *maxVal = (ConstUInt)SCHAR_MAX;
        return;
    case PrimShort:
        *isUnsigned = false;
        *minVal = SHRT_MIN;
        *maxVal = (ConstUInt)SHRT_MAX;
        return;
    case PrimInt:
        *isUnsigned = false;
        *minVal = INT_MIN;
        *maxVal = (ConstUInt)INT_MAX;
        return;
    default:;
    }

    // "long" (and any fallback)
    *isUnsigned = false;
    *minVal = LLONG_MIN;
    *maxVal = (ConstUInt)LLONG_MAX;
}

// Interprets `mag` (with sign) as a signed value; false when the magnitude
// cannot be represented in int64 (only reachable for `long` underlying).
bool EnumSignedValue(ConstUInt mag, bool isNegative, ConstInt* out)
{
    if (isNegative)
    {
        if (mag == (1ULL << 63))
        {
            *out = INT64_MIN;
            return true;
        }

        if (mag > (ConstUInt)INT64_MAX)
        {
            return false;
        }

        *out = -(ConstInt)mag;
        return true;
    }

    if (mag > (ConstUInt)INT64_MAX)
    {
        return false;
    }

    *out = (ConstInt)mag;
    return true;
}

// Extracts a compile-time integer value from a RESOLVED constant expression:
// an integer literal, a +/- literal, or an enum constant.
bool ResolvedConstIntValue(Node* n, ConstUInt* mag, bool* isNegative)
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

    // `int.max` / `float.min`: integral pseudo-properties fold to their
    // bit pattern (floats are not integer constants).
    if (n->kind == NodeMember && ((MemberExpr*)n)->isScalarConst)
    {
        MemberExpr* m = (MemberExpr*)n;
        ConstUInt intVal = 0;
        bool isFloat = false;

        if (m->baseNode->kind == NodeIdent
            && ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)m->baseNode)->name), m->member, &intVal, NULL, &isFloat)
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

// Range-checks a constant value against a scalar integral type name.
bool EnumConstFits(const char* underlying, ConstUInt mag, bool isNegative)
{
    bool isUnsigned = false;
    ConstInt minVal = 0;
    ConstUInt maxVal = 0;
    EnumSignedRange(underlying, &isUnsigned, &minVal, &maxVal);

    if (isUnsigned)
    {
        return !isNegative && mag <= maxVal;
    }

    ConstInt sv = 0;
    return EnumSignedValue(mag, isNegative, &sv) && sv >= minVal && (ConstUInt)sv <= maxVal;
}

// Does the constant (magnitude `mag`, sign `negative`) fit the integer type named `typeName`?
static bool IntegerConstFits(const char* typeName, ConstUInt mag, bool negative)
{
    bool isUnsigned = false;
    ConstInt minVal = 0;
    ConstUInt maxVal = 0;
    EnumSignedRange(typeName, &isUnsigned, &minVal, &maxVal);

    // A negative constant fits when its magnitude is within |min|; a positive one within max.
    return negative ? (!isUnsigned && mag <= (ConstUInt)(-(minVal + 1)) + 1ULL) : mag <= maxVal;
}

// A literal that initializes a scalar must fit it: `byte b = 300`, `uint u = -1` and `float f = 1e40` are errors
// rather than silent truncation or infinity. Only plain literals (and a negated one) are checked here.
void CheckLiteralFits(DiagnosticEngine* diag, const Node* value, const TypeName* target)
{
    if (!value || !target || !target->name || !TypeNameIsBare(target))
    {
        return;
    }

    PrimitiveType prim = GetPrimitiveType(target->name);
    bool integral = prim == PrimInt || prim == PrimUInt || prim == PrimLong || prim == PrimULong || prim == PrimSByte
                    || prim == PrimByte || prim == PrimShort || prim == PrimUShort;
    ConstUInt mag = 0;
    bool negative = false;
    const Node* lit = value;

    if (value->kind == NodeUnary && ((const UnaryExpr*)value)->op == UnNeg)
    {
        lit = ((const UnaryExpr*)value)->operand;
    }

    if (integral && lit->kind == NodeIntLiteral && ResolvedConstIntValue((Node*)value, &mag, &negative))
    {
        if (!IntegerConstFits(target->name, mag, negative))
        {
            DiagErrorFmt(diag, value->range, "integer constant %s%" PRIu64 " does not fit in '%s'", negative ? "-" : "",
                         mag, target->name);
        }

        return;
    }

    if (prim == PrimFloat && lit->kind == NodeFloatLiteral)
    {
        double v = ((const FloatLiteral*)lit)->value;

        if (v > 3.4028234663852886e38 || v < -3.4028234663852886e38)
        {
            DiagErrorFmt(diag, value->range, "float constant is out of range for 'float'");
        }
    }
}

// The value of a constant integer expression built from literals, negation, parentheses and + - *; false when
// it is anything else (or overflows 64 bits).
static bool EvalConstInt(const Node* n, ConstInt* out)
{
    if (n->kind == NodeIntLiteral)
    {
        *out = (ConstInt)((const IntLiteral*)n)->value;
        return true;
    }

    if (n->kind == NodeUnary && ((const UnaryExpr*)n)->op == UnNeg)
    {
        ConstInt v = 0;

        if (!EvalConstInt(((const UnaryExpr*)n)->operand, &v))
        {
            return false;
        }

        *out = -v;
        return true;
    }

    if (n->kind == NodeBinary)
    {
        const BinaryExpr* b = (const BinaryExpr*)n;
        ConstInt l = 0;
        ConstInt r = 0;

        if ((b->op != BinAdd && b->op != BinSub && b->op != BinMul) || !EvalConstInt(b->lhs, &l)
            || !EvalConstInt(b->rhs, &r))
        {
            return false;
        }

        *out = b->op == BinAdd ? l + r : b->op == BinSub ? l - r : l * r;
        return true;
    }

    return false;
}

// Bit width of an integer scalar type, 0 for anything else.
static BitWidth ScalarBitWidth(PrimitiveType p)
{
    switch (p)
    {
    case PrimSByte:
    case PrimByte:
        return 8;
    case PrimShort:
    case PrimUShort:
        return 16;
    case PrimInt:
    case PrimUInt:
        return 32;
    case PrimLong:
    case PrimULong:
        return 64;
    default:
        return 0;
    }
}

// Constant operands that can only trap at run time are compile errors: `7 / 0`, `5 % 0`, `1 << 40`,
// `INT_MIN / -1`. `lt` is the left operand's type.
void CheckConstArith(DiagnosticEngine* diag, const BinaryExpr* b, const TypeName* lt)
{
    ConstInt rv = 0;

    if (!EvalConstInt(b->rhs, &rv))
    {
        return;
    }

    if (b->op == BinDiv || b->op == BinMod)
    {
        ConstInt lv = 0;
        BitWidth width = lt ? ScalarBitWidth(lt->primitiveType) : 0;
        bool isUnsigned = lt
                          && (lt->primitiveType == PrimUInt || lt->primitiveType == PrimULong
                              || lt->primitiveType == PrimByte || lt->primitiveType == PrimUShort);

        if (rv == 0)
        {
            DiagErrorFmt(diag, b->base.range, "%s by zero", b->op == BinDiv ? "division" : "modulo");
        }
        else if (rv == -1 && !isUnsigned && width && EvalConstInt(b->lhs, &lv)
                 && lv == (width == 64 ? LLONG_MIN : -(1LL << (width - 1))))
        {
            DiagErrorFmt(diag, b->base.range, "%s overflows: the minimum value cannot be divided by -1",
                         b->op == BinDiv ? "division" : "modulo");
        }
    }
    else if (b->op == BinShl || b->op == BinShr)
    {
        BitWidth width = lt ? ScalarBitWidth(lt->primitiveType) : 0;

        if (width && (rv < 0 || rv >= (ConstInt)width))
        {
            DiagErrorFmt(diag, b->base.range, "shift count %" PRId64 " is not less than the %u-bit width of '%s'", rv,
                         width, lt->name);
        }
    }
}

// `(int)3.0e10`: a constant float that cannot be represented in the integer type it is cast to.
void CheckConstFloatToInt(DiagnosticEngine* diag, const CastExpr* cast)
{
    const Node* op = cast->operand;
    bool negative = false;

    if (op->kind == NodeUnary && ((const UnaryExpr*)op)->op == UnNeg)
    {
        negative = true;
        op = ((const UnaryExpr*)op)->operand;
    }

    if (op->kind != NodeFloatLiteral || !cast->type.name)
    {
        return;
    }

    PrimitiveType prim = GetPrimitiveType(cast->type.name);
    BitWidth width = ScalarBitWidth(prim);

    if (!width)
    {
        return;
    }

    bool isUnsigned = prim == PrimUInt || prim == PrimULong || prim == PrimByte || prim == PrimUShort;
    double v = ((const FloatLiteral*)op)->value * (negative ? -1.0 : 1.0);
    double lo = isUnsigned ? 0.0 : -ldexp(1.0, (int)width - 1);
    double hi = isUnsigned ? ldexp(1.0, (int)width) : ldexp(1.0, (int)width - 1);

    if (!(v > lo - 1.0 && v < hi))
    {
        DiagErrorFmt(diag, cast->base.range, "constant %g does not fit in '%s'", v, cast->type.name);
    }
}

// Does every value of `src` survive conversion to `dst`? Widening within a signedness, an unsigned type into a
// wider signed one, and exact conversions to float/double. Nothing else converts silently.
static bool NumericConversionIsLossless(PrimitiveType src, PrimitiveType dst)
{
    if (src == dst)
    {
        return true;
    }

    switch (src)
    {
    case PrimSByte:
        return dst == PrimShort || dst == PrimInt || dst == PrimLong || dst == PrimFloat || dst == PrimDouble;
    case PrimByte:
        return dst == PrimShort || dst == PrimUShort || dst == PrimInt || dst == PrimUInt || dst == PrimLong
               || dst == PrimULong || dst == PrimFloat || dst == PrimDouble;
    case PrimShort:
        return dst == PrimInt || dst == PrimLong || dst == PrimFloat || dst == PrimDouble;
    case PrimUShort:
        return dst == PrimInt || dst == PrimUInt || dst == PrimLong || dst == PrimULong || dst == PrimFloat
               || dst == PrimDouble;
    case PrimInt:
        return dst == PrimLong || dst == PrimDouble;
    case PrimUInt:
        return dst == PrimLong || dst == PrimULong || dst == PrimDouble;
    case PrimFloat:
        return dst == PrimDouble;
    default:
        return false;
    }
}

// A value that would lose information in an implicit numeric conversion needs a cast. A literal is judged by
// its value (CheckLiteralFits), so `byte b = 200` and `float f = 1.5` stay legal.
void CheckLossyConversion(DiagnosticEngine* diag, const Node* value, const TypeName* srcType, const TypeName* target,
                          SourceRange range)
{
    if (!value || !srcType || !target || !TypeNameIsBare(target) || !TypeNameIsBare(srcType) || !srcType->name
        || !target->name)
    {
        return;
    }

    PrimitiveType src = GetPrimitiveType(srcType->name);
    PrimitiveType dst = GetPrimitiveType(target->name);
    bool srcNum = IsNumeric(src) && src != PrimBool;
    bool dstNum = IsNumeric(dst) && dst != PrimBool;

    if (!srcNum || !dstNum || NumericConversionIsLossless(src, dst))
    {
        return;
    }

    // Constants are judged by value, not by the type they were written in.
    ConstInt ignored = 0;
    const Node* lit = value;

    if (lit->kind == NodeUnary && ((const UnaryExpr*)lit)->op == UnNeg)
    {
        lit = ((const UnaryExpr*)lit)->operand;
    }

    if (lit->kind == NodeIntLiteral || lit->kind == NodeFloatLiteral || EvalConstInt(value, &ignored))
    {
        return;
    }

    DiagErrorFmt(diag, range, "implicit conversion from '%s' to '%s' may lose information; use a cast", srcType->name,
                 target->name);
}

// An argument converts to a scalar parameter silently only if nothing is lost: a widening conversion, or a
// constant whose value fits. Otherwise the overload is not a candidate (the caller can cast).
bool ArgConvertsWithoutLoss(const Node* arg, const TypeName* argType, const TypeName* paramType)
{
    PrimitiveType src = argType->primitiveType;
    PrimitiveType dst = paramType->primitiveType;

    if (!IsNumeric(src) || !IsNumeric(dst) || src == PrimBool || dst == PrimBool
        || NumericConversionIsLossless(src, dst))
    {
        return true;
    }

    const Node* lit = arg;
    bool negative = false;

    if (lit->kind == NodeUnary && ((const UnaryExpr*)lit)->op == UnNeg)
    {
        negative = true;
        lit = ((const UnaryExpr*)lit)->operand;
    }

    if (lit->kind == NodeIntLiteral)
    {
        if (dst == PrimFloat || dst == PrimDouble)
        {
            return true;
        }

        return IntegerConstFits(paramType->name, ((const IntLiteral*)lit)->value, negative);
    }

    // A float literal is written in no particular width: it fits `float`, never an integer.
    return lit->kind == NodeFloatLiteral && (dst == PrimFloat || dst == PrimDouble);
}

// Folds an integer operator at the type the runtime computes it in: both operands are converted to that type
// first, so `>>` picks the same shift the emitted code does.
static bool SemaFoldIntBinary(BinaryOp op, ConstGlobalVal l, ConstGlobalVal r, ConstGlobalVal* out)
{
    ConstIntType t = ConstIntTypeCommon(l.ity, r.ity);
    ConstUInt v;

    if (!ConstFoldIntAt(op, ConstIntNarrow((ConstUInt)l.i, t), ConstIntNarrow((ConstUInt)r.i, t), t, &v))
    {
        return false;
    }

    out->i = (ConstInt)v;
    out->f = 0.0;
    out->isFloat = false;
    out->ity = t;
    return true;
}

// Applies a binary operator to two folded values with C constant-expression
// semantics: usual arithmetic conversions (int promotes to double when the
// other side is float); bitwise/shift/modulo require integers; comparisons
// and logical ops yield 0/1.
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
            out->ity = (ConstIntType){0, false};
            return ConstFoldFloatArith(op, a, b, &out->f);
        }

        return SemaFoldIntBinary(op, l, r, out);
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

        return SemaFoldIntBinary(op, l, r, out);
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

            if (!ConstFoldFloatCompare(op, a, b, &res))
            {
                return false;
            }
        }
        else if (!ConstFoldIntCompare(op, l.i, r.i, &res))
        {
            return false;
        }

        out->i = res ? 1 : 0;
        out->f = 0.0;
        out->isFloat = false;
        out->ity = (ConstIntType){0, false};
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
        out->ity = (ConstIntType){0, false};
        return true;
    }
    default:
        return false;
    }
}

static bool FoldIntLiteral(Node* n, ConstGlobalVal* out)
{
    // Typed as the checker types it (InferType): `l` suffixes and values past i32 are 64-bit.
    const IntLiteral* lit = (const IntLiteral*)n;

    out->i = (ConstInt)lit->value;
    out->f = 0.0;
    out->isFloat = false;
    out->ity = (ConstIntType){lit->isLong || lit->value > 0x7FFFFFFFULL ? 64u : 32u, lit->isUnsigned};
    return true;
}

static bool FoldFloatLiteral(Node* n, ConstGlobalVal* out)
{
    out->f = ((FloatLiteral*)n)->value;
    out->isFloat = true;
    out->ity = (ConstIntType){0, false};
    return true;
}

static bool FoldBoolLiteral(Node* n, ConstGlobalVal* out)
{
    out->i = ((BoolLiteral*)n)->value ? 1 : 0;
    out->f = 0.0;
    out->isFloat = false;
    out->ity = (ConstIntType){0, false};
    return true;
}

static bool FoldUnary(Resolver* r, Node* n, ConstGlobalVal* out)
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
            out->i = (ConstInt)ConstIntNarrow((ConstUInt)(-out->i), out->ity);
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
        out->ity = (ConstIntType){0, false};
        return true;
    case UnBitNot:
        if (out->isFloat)
        {
            return false;
        }

        out->i = (ConstInt)ConstIntNarrow((ConstUInt)~out->i, out->ity);
        return true;
    }

    return false;
}

static bool FoldBinary(Resolver* r, Node* n, ConstGlobalVal* out)
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

static bool FoldCast(Resolver* r, Node* n, ConstGlobalVal* out)
{
    ConstGlobalVal inner = {0};

    if (!SemaFoldConstInit(r, ((CastExpr*)n)->operand, &inner))
    {
        return false;
    }

    // `isInt` describes the DECLARED type (set by the caller before the
    // fold); the value copy must not clobber it.
    bool wasInt = out->isInt;
    *out = inner;
    out->isInt = wasInt;

    // An integer target takes the value at its own width (`(byte)-8` is 248) and converts a float operand.
    const char* castName = ((CastExpr*)n)->type.name;
    ConstIntType target = {0, false};

    if (castName)
    {
        target = SemaConstIntType(GetPrimitiveType(TypeRegistryResolveAlias(&r->registry, castName)));
    }

    if (target.bits != 0)
    {
        if (out->isFloat)
        {
            out->i = (ConstInt)out->f;
            out->f = 0.0;
            out->isFloat = false;
        }

        out->ity = target;
        out->i = (ConstInt)ConstIntNarrow((ConstUInt)out->i, target);
    }

    return true;
}

static bool FoldIdent(Resolver* r, Node* n, ConstGlobalVal* out)
{
    ConstGlobalVal* prev = (ConstGlobalVal*)StrMapGet(&r->constGlobals, ((IdentExpr*)n)->name);

    if (!prev)
    {
        return false;
    }

    bool wasInt = out->isInt;
    *out = *prev;
    out->isInt = wasInt;
    return true;
}

static bool FoldMember(Resolver* r, Node* n, ConstGlobalVal* out)
{
    // `int.max` / `float.min` — builtin scalar pseudo-properties fold
    // to their constants, exactly like enum constants.
    MemberExpr* m = (MemberExpr*)n;

    if (m->baseNode->kind != NodeIdent)
    {
        return false;
    }

    ConstUInt intVal = 0;
    double floatVal = 0.0;
    bool isFloat = false;

    if (ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)m->baseNode)->name), m->member, &intVal, &floatVal, &isFloat))
    {
        bool wasInt = out->isInt;
        out->i = (ConstInt)intVal;
        out->f = floatVal;
        out->isFloat = isFloat;
        out->isInt = wasInt;
        out->ity = SemaConstIntType(GetPrimitiveType(((IdentExpr*)m->baseNode)->name));
        return true;
    }

    // `EnumName.Member` — a scoped enum constant folds to its value. Runs
    // BEFORE the main resolution marks members, so scan the module.
    if (!TypeRegistryIsEnum(&r->registry, ((IdentExpr*)m->baseNode)->name))
    {
        return false;
    }

    const char* baseName = ((IdentExpr*)m->baseNode)->name;

    for (size_t i = 0; i < r->mod->enums.count; i++)
    {
        EnumDecl* ed = (EnumDecl*)VecGet(&r->mod->enums, i);

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
                out->i = (ConstInt)member->value;
                out->f = 0.0;
                out->isFloat = false;
                out->isInt = wasInt;
                // An enum member is typed by its underlying integer type (`int` when none is spelled).
                out->ity = SemaConstIntType(GetPrimitiveType(
                    TypeRegistryResolveAlias(&r->registry, ed->underlyingType ? ed->underlyingType : "int")));
                return true;
            }
        }

        return false;
    }

    return false;
}

// Folds a const-global initializer to a compile-time value. Handles int/
// float/bool literals, unary ops, scalar casts, the C constant-expression
// operators over folded operands, and references to earlier manifest
// constants (`const B = A | (1 << 2);`).
bool SemaFoldConstInit(Resolver* r, Node* n, ConstGlobalVal* out)
{
    if (!n)
    {
        return false;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        return FoldIntLiteral(n, out);
    case NodeFloatLiteral:
        return FoldFloatLiteral(n, out);
    case NodeBoolLiteral:
        return FoldBoolLiteral(n, out);
    case NodeUnary:
        return FoldUnary(r, n, out);
    case NodeBinary:
        return FoldBinary(r, n, out);
    case NodeCast:
        return FoldCast(r, n, out);
    case NodeIdent:
        return FoldIdent(r, n, out);
    case NodeMember:
        return FoldMember(r, n, out);
    default:
        return false;
    }
}

// True when the (alias-resolved) type is an integer kind usable as a fixed
// array dimension.
bool IsConstDimType(const TypeRegistry* reg, const char* name)
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

// A `const` scalar global of any numeric type folds into the manifest table
// (floats/bools inline at uses too — they just cannot size arrays).
bool IsManifestConstType(const TypeRegistry* reg, const char* name)
{
    return IsNameNumeric(TypeRegistryResolveAlias(reg, name));
}

// Resolves `[constName]` fixed-array dimensions on a type tree against the
// manifest-constant table.
bool SemaResolveConstDims(Resolver* r, TypeName* t)
{
    if (!t)
    {
        return true;
    }

    if (t->lengthName)
    {
        ConstGlobalVal* v = (ConstGlobalVal*)StrMapGet(&r->constGlobals, t->lengthName);

        if (!v || !v->isInt)
        {
            DiagErrorFmt(r->diag, t->range,
                         "array dimension '%s' is not a compile-time integer constant "
                         "(declare it 'const int %s = ...;')",
                         t->lengthName, t->lengthName);
            return false;
        }

        t->length = (ArrayLength)v->i;
        char* dimName = t->lengthName;
        t->lengthName = NULL;

        if (t->length < 1)
        {
            DiagErrorFmt(r->diag, t->range, "array dimension '%s' must be at least 1 (got %ld)",
                         dimName ? dimName : "?", t->length);
            return false;
        }

        // Rebuild the canonical spelling with the resolved dimension.
        char* open = strchr(t->name, '[');

        if (open)
        {
            char* close = strchr(open, ']');

            if (close)
            {
                t->name = ArenaFormat(r->arena, "%.*s[%ld]%s", (int)(open - t->name), t->name, t->length, close + 1);
            }
        }
    }

    if (!SemaResolveConstDims(r, t->elem))
    {
        return false;
    }

    return true;
}

// Width of a constant expression: 32-bit if it bottoms out in a 32-bit literal.
BitWidth ConstBitWidth(const Node* n)
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
