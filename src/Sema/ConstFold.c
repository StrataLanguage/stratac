#include "Sema/ConstFold.h"

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
