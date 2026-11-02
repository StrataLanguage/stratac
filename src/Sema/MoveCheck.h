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

typedef enum {
    MoveDiagUse,        /* a moved or maybe-moved place is used */
    MoveDiagNarrow,     /* a `T?` is read without a proof that it is set */
    MoveDiagLoan,       /* an operand moves or touches a place an earlier operand has loaned */
    MoveDiagGlobalLoan, /* an argument borrows a global place the callee writes */
    MoveDiagElement,    /* an Owning value is moved out of an indexed element (not reported yet) */
    MoveDiagDropField,  /* a field is moved out of a struct with a Drop method */
} MoveDiagKind;

typedef struct {
    SourceRange range; /* the offending use */
    const char* name;
    MoveDiagKind kind;
} MoveDiag;

/* The global places each function of a module may write, for `MoveCheckFunction`. */
typedef struct WriteSets WriteSets;
WriteSets* WriteSetsBuild(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, Arena* arena);
void WriteSetsFree(WriteSets* ws);

/* Uses of a moved or maybe-moved place, and reads of an unproven `T?`, in `fn`. The caller frees `*out`. */
size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types,
                         const WriteSets* ws, Arena* arena, MoveDiag** out);

/* Runs MoveCheckFunction over every function of `mod`. What it finds that the old checker did not
   report (no "used after move", "poisoned" or "has not been blessed" error at that spot) is added to
   `diag`. With `compare`, every disagreement with the old checker's errors, in either direction, is also
   written to stderr as `shadow: only-new|only-old file:offset 'name'`; returns how many there were. */
size_t MoveCheckModule(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, DiagnosticEngine* diag,
                       Arena* arena, bool compare);
