// Strata compiler: code generation back-end interface.
//
// A back-end consumes a (successfully parsed) Module and produces output. The
// driver selects an implementation: the LLVM C API back-end builds the IR
// in-process through the linked LLVM-C shared library, while the text back-end
// emits human-readable LLVM IR as a string. Keeping this behind an interface
// lets us develop the front-end and IR generation independently of how native
// code is ultimately produced.
#pragma once

#include "strata/AST/AST.h"

#include <memory>
#include <string>
#include <string_view>

namespace strata {

enum class EmitKind {
    LLVMIR,   // textual LLVM IR (.ll)
    AST,      // a pretty-printed AST dump (for debugging/tooling)
};

struct CodegenResult {
    bool ok = false;
    std::string output;          // generated text (IR or AST dump)
    std::string moduleName;
};

class CodegenBackend {
public:
    virtual ~CodegenBackend() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual CodegenResult generate(const Module& mod) = 0;
};

// Factories. createTextBackend() always succeeds. createLLVMBackend() returns
// nullptr if Strata was built without LLVM linkage.
std::unique_ptr<CodegenBackend> createTextBackend();
std::unique_ptr<CodegenBackend> createLLVMBackend();

// Pretty-prints the AST as a human-readable tree (used by the AST emitter and
// tests). Indents with two spaces per level.
std::string dumpAST(const Module& mod);

} // namespace strata
