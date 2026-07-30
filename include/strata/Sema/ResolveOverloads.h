// Strata compiler: overload resolution (the project's first semantic pass).
//
// Strata allows multiple functions to share a name when their parameter types
// differ. This pass:
//   1. Assigns each function a unique IR symbol (mangled when overloaded).
//   2. Walks every call site, infers the argument types, and rewrites the call
//      to the best-matching overload (exact match preferred; numeric
//      conversions allowed with lower priority).
//   3. Reports diagnostics for ambiguous or unresolvable calls and for illegal
//      overload sets (extern functions cannot be overloaded).
//
// After this pass, FunctionDecl::mangledName and CallExpr::callee (mangled)
// plus CallExpr::resolvedDecl are populated, so the code generators can emit a
// call to a single, unambiguous symbol without re-doing resolution.
#pragma once

#include "strata/AST/AST.h"
#include "strata/Core/Diagnostics.h"

namespace strata {

void resolveOverloads(Module& mod, DiagnosticEngine& diag);

} // namespace strata
