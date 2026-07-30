#ifdef STRATA_ENABLE_LLVM
#include "LLVMModuleBuilder.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Codegen/LLVMCApi.h"

#include <string>

namespace strata
{

namespace
{
using namespace strata::llvm_c;

class LLVMCBackend : public CodegenBackend
{
  public:
    std::string_view Name() const noexcept override
    {
        return "llvm-c";
    }

    CodegenResult Generate(const Module& mod) override
    {
        CodegenResult res;
        res.moduleName = mod.name;
        std::string notes;
        BuiltModule bm = BuildLlvmModule(mod, notes);
        char* ir = LLVMPrintModuleToString(bm.mod);
        res.output = notes + ir;
        LLVMDisposeMessage(ir);
        res.ok = true;
        return res;
    }
};
} // namespace

std::unique_ptr<CodegenBackend> CreateLlvmBackend()
{
    return std::make_unique<LLVMCBackend>();
}

} // namespace strata

#endif // STRATA_ENABLE_LLVM
