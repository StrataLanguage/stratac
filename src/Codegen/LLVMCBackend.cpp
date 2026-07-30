#ifdef STRATA_ENABLE_LLVM
#include "LLVMModuleBuilder.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Codegen/LLVMCApi.h"
#include "strata/Core/Diagnostics.h"

#include <sstream>
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

        DiagnosticEngine diag;
        std::string notes;

        BuiltModule bm = BuildLlvmModule(mod, diag, notes);

        if (diag.HasErrors())
        {
            std::ostringstream msg;
            msg << "; LLVM back-end errors:\n";
            for (const auto& d : diag.Diagnostics())
            {
                msg << ";   " << d.message << "\n";
            }

            res.output = msg.str();
            res.ok = false;
            return res;
        }

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
