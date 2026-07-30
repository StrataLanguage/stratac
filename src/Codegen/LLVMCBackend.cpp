// Strata compiler: in-process LLVM back-end (textual IR) via the C API.
//
// This back-end prints LLVM IR. The IR construction itself lives in
// LLVMModuleBuilder so it can be shared with the AOT emitter (LLVMAot) and the
// JIT (LLVMJit). Only compiled when STRATA_ENABLE_LLVM is defined; otherwise
// the factory falls back to the null stub in CodegenBackend.cpp.
#if defined(STRATA_ENABLE_LLVM)
#include "strata/Codegen/CodegenBackend.h"
#include "LLVMModuleBuilder.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"

#include <string>

namespace strata {

namespace {
using namespace strata::llvm_c;

class LLVMCBackend : public CodegenBackend {
public:
    std::string_view name() const noexcept override { return "llvm-c"; }
    CodegenResult generate(const Module& mod) override {
        CodegenResult res;
        res.moduleName = mod.name;
        std::string notes;
        BuiltModule bm = buildLLVMModule(mod, notes);
        char* ir = LLVMPrintModuleToString(bm.mod);
        res.output = notes + ir;
        LLVMDisposeMessage(ir);
        res.ok = true;
        return res;
    }
};
} // namespace

std::unique_ptr<CodegenBackend> createLLVMBackend() {
    return std::make_unique<LLVMCBackend>();
}

} // namespace strata

#endif // STRATA_ENABLE_LLVM
