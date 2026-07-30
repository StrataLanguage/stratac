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
using namespace strata::llvm_c;

CodegenResult GenerateLlvmIr(const Module& mod)
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

} // namespace strata

#endif // STRATA_ENABLE_LLVM
