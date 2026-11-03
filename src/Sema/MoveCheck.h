#pragma once
// Move, narrowing and loan checks over the CFG of each function.

#include "AST/AST.h"
#include "Codegen/TypeRegistry.h"
#include "Core/Diagnostics.h"
#include "Sema/NodeTypes.h"

// Moves are tracked per place path (`a`, `a.b`) for Owning locals and by-value params. Narrowing
// proves `T?` places non-empty (null tests, sets from non-empty values) and flags reads that lack the
// proof. Loans: a place an earlier operand of a call, operator or index takes stays loaned to the end of
// the expression, and later operands may not move, mutate or (for a mutable loan) touch it.

typedef enum
{
    MoveDiagUse,        // a moved or maybe-moved place is used
    MoveDiagPoisoned,   // a place is used whole while a part of it (`detail`) is moved out
    MoveDiagNarrow,     // a `T?` is read without a proof that it is set
    MoveDiagLoan,       // an operand moves or touches a place an earlier operand has loaned
    MoveDiagGlobalLoan, // an argument borrows a global place the callee writes
    MoveDiagElement,    // an Owning value is moved out of an indexed element (not reported yet)
    MoveDiagDropField,  // a field is moved out of a struct with a Drop method
    MoveDiagRefParam,   // a ref parameter still has a part moved out when the function returns
    MoveDiagDangle,     // a `ref` is used after the place it refers to was written, moved or reallocated
} MoveDiagKind;

typedef struct
{
    SourceRange range; // the offending use
    const char* name;
    MoveDiagKind kind;
    const char* detail; // the moved part, or "definitely empty" for a read of a place known to be empty
    const char* type;   // MoveDiagNarrow: the optional's type, as the message spells it
} MoveDiag;

// The global places each function of a module may write, for `MoveCheckFunction`.
typedef struct WriteSets WriteSets;
WriteSets* WriteSetsBuild(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, Arena* arena);
void WriteSetsFree(WriteSets* ws);

// Uses of a moved or maybe-moved place, and reads of an unproven `T?`, in `fn`. The caller frees `*out`.
size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types, const WriteSets* ws,
                         Arena* arena, MoveDiag** out);

// Runs MoveCheckFunction over every function of `mod` and adds what it finds to `diag`.
void MoveCheckModule(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, DiagnosticEngine* diag,
                     Arena* arena);
