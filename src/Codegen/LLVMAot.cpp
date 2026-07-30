#include "LLVMAot.h"
#include "strata/Codegen/LLVMCApi.h"

#include <mutex>

namespace strata
{

static void EnsureTargetsInitialized()
{
    using namespace strata::llvm_c;

    static std::once_flag flag;
    
    std::call_once(
        flag,
        []
        {
            LLVMInitializeX86TargetInfo();
            LLVMInitializeX86Target();
            LLVMInitializeX86TargetMC();
            LLVMInitializeX86AsmPrinter();

            LLVMInitializeAArch64TargetInfo();
            LLVMInitializeAArch64Target();
            LLVMInitializeAArch64TargetMC();
            LLVMInitializeAArch64AsmPrinter();
        });
}

bool EmitNativeFile(const BuiltModule& bm, const std::string& path, bool assembly, std::string& errorMessage,
                    const std::string& targetTriple)
{
    if (!bm.mod)
    {
        errorMessage = "no module to emit";

        return false;
    }

    EnsureTargetsInitialized();

    // Use the explicit triple when provided, otherwise fall back to the host.
    const char* triple = nullptr;
    char* defaultTriple = nullptr;

    if (!targetTriple.empty())
    {
        triple = targetTriple.c_str();
    }
    else
    {
        defaultTriple = LLVMGetDefaultTargetTriple();
        triple = defaultTriple;
    }

    LLVMTargetRef target = nullptr;
    
    char* error = nullptr;

    if (LLVMGetTargetFromTriple(triple, &target, &error))
    {
        errorMessage = std::string("unknown target triple '") + triple + "': " + (error ? error : "(no message)");

        if (error)
        {
            LLVMDisposeMessage(error);
        }

        if (defaultTriple)
        {
            LLVMDisposeMessage(defaultTriple);
        }

        return false;
    }

    LLVMTargetMachineRef targetMachine = LLVMCreateTargetMachine(
        target,
        triple,
        "" /* host CPU */,
        "" /* features */,
        LLVMCodeGenLevelDefault,
        LLVMRelocDefault,
        LLVMCodeModelDefault);

    if (defaultTriple)
    {
        LLVMDisposeMessage(defaultTriple);
    }

    if (!targetMachine)
    {
        errorMessage = "could not create target machine";

        return false;
    }

    // Stamp the module with the target triple and data layout so the IR matches
    // the chosen ABI (pointer width, alignment, etc.). The TargetMachine would
    // override these during emission anyway, but setting them explicitly avoids
    // verifier warnings and makes --emit ir consistent with --emit obj.
    LLVMSetTarget(bm.mod, triple);

    LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(targetMachine);
    char* dataLayoutStr = LLVMCopyStringRepOfTargetData(dataLayout);
    LLVMSetDataLayout(bm.mod, dataLayoutStr);
    LLVMDisposeMessage(dataLayoutStr);
    LLVMDisposeTargetData(dataLayout);

    LLVMCodeGenFileType kind = assembly ? LLVMAssemblyFile : LLVMObjectFile;
    char* emitError = nullptr;
    bool ok = !LLVMTargetMachineEmitToFile(targetMachine, bm.mod, path.c_str(), kind, &emitError);

    if (!ok)
    {
        errorMessage = std::string("emission failed: ") + (emitError ? emitError : "(no message)");
    }

    if (emitError)
    {
        LLVMDisposeMessage(emitError);
    }

    LLVMDisposeTargetMachine(targetMachine);

    return ok;
}

} // namespace strata
