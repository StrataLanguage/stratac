#pragma once

#include "AST/AST.h"

#include <stdbool.h>
#include <stdint.h>

/* Leaf operators for compile-time constant folding. Sema (ResolveOverloads.c, ConstGlobalVal) and codegen
   (LLVMModuleBuilder.c, ConstInitVal) each keep their own value type and their own walk over the
   expression tree; both call these for the operator itself. Each function returns false when the fold must
   be refused, and writes its result only on success. */

/* How `>>` treats the sign bit of its left operand. Sema folds a logical shift of the 64-bit pattern;
   codegen folds an arithmetic shift. The two differ for negative operands; both are kept as they are. */
typedef enum
{
    ConstShrLogical,
    ConstShrArithmetic,
} ConstShrKind;

/* Two's-complement arithmetic on 64-bit patterns. `/` and `%` use C signed semantics. Refuses division or
   modulo by zero, and shifts whose count is >= 64 (a negative count reads as one). */
bool ConstFoldIntArith(BinaryOp op, uint64_t a, uint64_t b, ConstShrKind shr, uint64_t* out);

/* `+ - * /` on doubles. Refuses division by zero so the fold never embeds an inf or nan literal. */
bool ConstFoldFloatArith(BinaryOp op, double a, double b, double* out);

/* Signed 64-bit comparisons. */
bool ConstFoldIntCompare(BinaryOp op, int64_t a, int64_t b, bool* res);

/* Double comparisons. */
bool ConstFoldFloatCompare(BinaryOp op, double a, double b, bool* res);
