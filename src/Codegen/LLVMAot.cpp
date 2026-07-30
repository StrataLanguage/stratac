// Strata compiler: AOT native emission implementation.
#include "LLVMAot.h"
#include "strata/Codegen/LLVMCApi.h"

#include <mutex>

namespace strata
{

namespace
{
using namespace strata::llvm_c;

// The X86 target must be initialized exactly once before any TargetMachine or
// ExecutionEngine is created. (The LLVMInitializeAll* wrappers are not exported
// by this LLVM-C.dll; the X86-specific entry points are, and the host is x86_64.)
void EnsureX86Initialized()
{
    static std::once_flag flag;
    std::call_once(flag,
                   []
                   {
                       LLVMInitializeX86TargetInfo();
                       LLVMInitializeX86Target();
                       LLVMInitializeX86TargetMC();
                       LLVMInitializeX86AsmPrinter();
                   });
}
} // namespace

bool EmitNativeFile(const BuiltModule& bm, const std::string& path, bool assembly, std::string& errorMessage)
{
    if (!bm.mod)
    {
        errorMessage = "no module to emit";
        return false;
    }

    EnsureX86Initialized();

    char* triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = nullptr;
    char* error = nullptr;
    if (LLVMGetTargetFromTriple(triple, &target, &error))
    {
        errorMessage = std::string("unknown target triple '") + triple + "': " + (error ? error : "(no message)");
        if (error)
        {
            LLVMDisposeMessage(error);
        }

        LLVMDisposeMessage(triple);
        return false;
    }

    LLVMTargetMachineRef targetMachine =
        LLVMCreateTargetMachine(target, triple, "" /* host CPU */, "" /* features */, LLVMCodeGenLevelDefault,
                                LLVMRelocDefault, LLVMCodeModelDefault);
    LLVMDisposeMessage(triple);
    if (!targetMachine)
    {
        errorMessage = "could not create target machine";
        return false;
    }

    LLVMCodeGenFileType kind = assembly ? LLVMAssemblyFile : LLVMObjectFile;
    char* emitErr = nullptr;
    bool ok = !LLVMTargetMachineEmitToFile(targetMachine, bm.mod, path.c_str(), kind, &emitErr);
    if (!ok)
    {
        errorMessage = std::string("emission failed: ") + (emitErr ? emitErr : "(no message)");
    }

    if (emitErr)
    {
        LLVMDisposeMessage(emitErr);
    }

    LLVMDisposeTargetMachine(targetMachine);
    return ok;
}

} // namespace strata
