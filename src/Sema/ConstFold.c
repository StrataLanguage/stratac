#include "Sema/ConstFold.h"

ConstIntType ConstIntTypeCommon(ConstIntType l, ConstIntType r)
{
    return r.bits > l.bits ? r : l;
}

uint64_t ConstIntNarrow(uint64_t v, ConstIntType t)
{
    if (t.bits == 0 || t.bits >= 64)
    {
        return v;
    }

    uint64_t mask = (1ULL << t.bits) - 1;
    uint64_t low = v & mask;

    if (!t.isUnsigned && (low >> (t.bits - 1)) != 0)
    {
        return low | ~mask;
    }

    return low;
}

static ConstShrKind ConstShrKindOf(ConstIntType t)
{
    return t.isUnsigned ? ConstShrLogical : ConstShrArithmetic;
}

bool ConstFoldIntAt(BinaryOp op, uint64_t a, uint64_t b, ConstIntType t, uint64_t* out)
{
    unsigned width = t.bits ? t.bits : 64;

    if ((op == BinShl || op == BinShr) && b >= width)
    {
        return false;
    }

    if (!ConstFoldIntArith(op, a, b, ConstShrKindOf(t), out))
    {
        return false;
    }

    *out = ConstIntNarrow(*out, t);
    return true;
}

/* Two's-complement integer arithmetic. `/`, `%` and `>>` apply C signed semantics, except that `>>` is a
   logical shift of the bit pattern under ConstShrLogical. Refuses division or modulo by zero, and shifts whose
   count is >= 64. */
bool ConstFoldIntArith(BinaryOp op, uint64_t a, uint64_t b, ConstShrKind shr, uint64_t* out)
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
        if (b == 0)
        {
            return false;
        }

        *out = (uint64_t)((int64_t)a / (int64_t)b);
        return true;
    case BinMod:
        if (b == 0)
        {
            return false;
        }

        *out = (uint64_t)((int64_t)a % (int64_t)b);
        return true;
    case BinBitAnd:
        *out = a & b;
        return true;
    case BinBitOr:
        *out = a | b;
        return true;
    case BinBitXor:
        *out = a ^ b;
        return true;
    case BinShl:
        if (b >= 64)
        {
            return false;
        }

        *out = a << b;
        return true;
    case BinShr:
        if (b >= 64)
        {
            return false;
        }

        if (shr == ConstShrArithmetic)
        {
            *out = (uint64_t)((int64_t)a >> b);
        }
        else
        {
            *out = a >> b;
        }

        return true;
    default:
        return false;
    }
}

/* Float constant arithmetic (`+ - * /`); refuses division by zero so the fold never embeds an inf/nan literal. */
bool ConstFoldFloatArith(BinaryOp op, double a, double b, double* out)
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

bool ConstFoldIntCompare(BinaryOp op, int64_t a, int64_t b, bool* res)
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

bool ConstFoldFloatCompare(BinaryOp op, double a, double b, bool* res)
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
