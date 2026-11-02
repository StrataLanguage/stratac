#pragma once

#include "AST/AST.h"
#include "Codegen/TypeRegistry.h"
#include "Core/Diagnostics.h"
#include "Sema/NodeTypes.h"

/* Roadmap phase 4, shadow mode: a move checker over the CFG that never reports an error. It tracks
   whole-variable moves of Owning locals and by-value params (no fields, elements or refs yet) and
   prints where it disagrees with the diagnostics the old checker produced. */

typedef struct {
    SourceRange range; /* the use of the moved variable */
    const char* name;
} MoveDiag;

/* Uses of a moved or maybe-moved variable in `fn`. The caller frees `*out`. */
size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types, Arena* arena,
                         MoveDiag** out);

/* Runs MoveCheckFunction over every function of `mod` and compares with the "used after move" errors
   in `diag`, writing each disagreement to stderr as `shadow: only-new|only-old file+offset 'name'`.
   Returns the number of disagreements. */
size_t MoveCheckShadow(const Module* mod, const TypeRegistry* reg, const NodeTypes* types,
                       const DiagnosticEngine* diag, Arena* arena);
