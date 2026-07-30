#pragma once

#include "strata/AST/AST.h"

#include <memory>
#include <string>
#include <string_view>

namespace strata
{

enum class EmitKind
{
    LLVMIR, // textual LLVM IR (.ll)
    AST,    // a pretty-printed AST dump (for debugging/tooling)
};

struct CodegenResult
{
    bool ok = false;
    std::string output; // generated text (IR or AST dump)
    std::string moduleName;
};

class CodegenBackend
{
  public:
    virtual ~CodegenBackend() = default;

    virtual std::string_view Name() const noexcept = 0;
    virtual CodegenResult Generate(const Module& mod) = 0;
};

// Factory. CreateLlvmBackend() returns nullptr if Strata was built without
// LLVM linkage.
std::unique_ptr<CodegenBackend> CreateLlvmBackend();

// Pretty-prints the AST as a human-readable tree (used by the AST emitter and
// tests). Indents with two spaces per level.
std::string DumpAst(const Module& mod);

} // namespace strata
