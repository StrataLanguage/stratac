#pragma once
// Constant-folding operators shared by Sema and codegen.

#include "AST/AST.h"
#include "Core/Types.h"

#include <stdbool.h>
#include <stdint.h>

// Leaf operators for compile-time constant folding. Sema (Resolve/ConstEval.c, ConstGlobalVal) and codegen
// (Codegen/Builder, ConstInitVal) each keep their own value type and their own walk over the
// expression tree; both call these for the operator itself. Each function returns false when the fold must
// be refused, and writes its result only on success.

// How `>>` treats the sign bit of its left operand: logical for an unsigned operand type, arithmetic for a signed
// one. It follows the signedness of the operation's type, the one EmitBinary computes in: the left operand's,
// unless the right operand is wider.
typedef enum
{
    ConstShrLogical,
    ConstShrArithmetic,
} ConstShrKind;

// The integer type a folded value has at runtime. A value's 64-bit pattern is always kept normalized to its type:
// sign-extended for a signed type, zero-extended for an unsigned one, truncated to `bits`. bits == 0 or 64 is no
// narrowing, which is also what non-integer values carry.
typedef struct
{
    BitWidth bits;
    bool isUnsigned;
} ConstIntType;

// The type a binary operator computes in, as EmitBinary picks it: the wider operand's type, and the left one's
// on a tie.
ConstIntType ConstIntTypeCommon(ConstIntType l, ConstIntType r);

// Truncates `v` to `t`'s width and extends it back to 64 bits, as the runtime value of that type reads.
ConstUInt ConstIntNarrow(ConstUInt v, ConstIntType t);

// Two's-complement arithmetic on 64-bit patterns. `/` and `%` use C signed semantics. Refuses division or
// modulo by zero, and shifts whose count is >= 64 (a negative count reads as one).
bool ConstFoldIntArith(BinaryOp op, ConstUInt a, ConstUInt b, ConstShrKind shr, ConstUInt* out);

// An integer binary operator computed at type `t`, the way the runtime computes it: `a` and `b` are the operands
// already converted to `t`, the shift kind follows `t`'s signedness, a shift count must be below `t`'s width, and
// the result is kept at `t`'s width.
bool ConstFoldIntAt(BinaryOp op, ConstUInt a, ConstUInt b, ConstIntType t, ConstUInt* out);

// `+ - * /` on doubles. Refuses division by zero so the fold never embeds an inf or nan literal.
bool ConstFoldFloatArith(BinaryOp op, double a, double b, double* out);

bool ConstFoldIntCompare(BinaryOp op, ConstInt a, ConstInt b, bool* res);

bool ConstFoldFloatCompare(BinaryOp op, double a, double b, bool* res);
