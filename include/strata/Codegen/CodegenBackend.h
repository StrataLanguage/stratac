#pragma once

#include "strata/AST/AST.h"

#include <string>

namespace strata
{

struct CodegenResult
{
    bool ok = false;
    std::string output;
    std::string moduleName;
};

// Builds the module via the LLVM C API and returns the textual LLVM IR.
// Returns {ok=false} if built without LLVM linkage or on a code-gen error.
CodegenResult GenerateLlvmIr(const Module& mod);

// Pretty-prints the AST as a human-readable tree (used by the AST emitter and
// tests). Indents with two spaces per level.
std::string DumpAst(const Module& mod);

} // namespace strata
