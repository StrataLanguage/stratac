// Target setup for the LLVM backends.
#include "LLVMTargets.h"
#include "Codegen/LLVMCApi.h"

#include <stdbool.h>

void EnsureTargetsInitialized(void)
{
    static bool initialized = false;

    if (!initialized)
    {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();

        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();

        initialized = true;
    }
}
