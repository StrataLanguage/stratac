#pragma once

#include "AST/AST.h"
#include "Codegen/TypeRegistry.h"
#include "Core/Diagnostics.h"
#include "Sema/NodeTypes.h"

/* Roadmap phase 4, shadow mode: move and narrowing checks over the CFG that never report an error.
   Moves are tracked per place path (`a`, `a.b`) for Owning locals and by-value params. Narrowing
   proves `T?` places non-empty (null tests, sets from non-empty values) and flags reads that lack the
   proof. Loans: a place an earlier operand of a call, operator or index takes stays loaned to the end of
   the expression, and later operands may not move, mutate or (for a mutable loan) touch it. All print where they disagree with the diagnostics the old checker produced. */

typedef struct {
    SourceRange range; /* the use of the moved variable */
    const char* name;
} MoveDiag;

/* The global places each function of a module may write, for `MoveCheckFunction`. */
typedef struct WriteSets WriteSets;
WriteSets* WriteSetsBuild(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, Arena* arena);
void WriteSetsFree(WriteSets* ws);

/* Uses of a moved or maybe-moved place, and reads of an unproven `T?`, in `fn`. The caller frees `*out`. */
size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types,
                         const WriteSets* ws, Arena* arena, MoveDiag** out);

/* Runs MoveCheckFunction over every function of `mod` and compares with the "used after move" errors
   (and "has not been blessed" ones) in `diag`, writing each disagreement to stderr as `shadow: only-new|only-old file+offset 'name'`.
   Returns the number of disagreements. */
size_t MoveCheckShadow(const Module* mod, const TypeRegistry* reg, const NodeTypes* types,
                       const DiagnosticEngine* diag, Arena* arena);
