#ifdef STRATA_ENABLE_LLVM

#include "LLVMAot.h"
#include "strata/Codegen/LLVMCApi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void EnsureTargetsInitialized(void)
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

bool EmitNativeFile(BuiltModule* bm, const char* path, bool assembly, char** errorMessage,
                    const char* targetTriple)
{
    if (!bm->mod)
    {
        *errorMessage = strdup("no module to emit");

        return false;
    }

    EnsureTargetsInitialized();

    const char* triple = NULL;
    char* defaultTriple = NULL;

    if (targetTriple && targetTriple[0] != '\0')
    {
        triple = targetTriple;
    }
    else
    {
        defaultTriple = LLVMGetDefaultTargetTriple();
        triple = defaultTriple;
    }

    LLVMTargetRef target = NULL;

    char* error = NULL;

    if (LLVMGetTargetFromTriple(triple, &target, &error))
    {
        const char* msg = error ? error : "(no message)";
        int needed = snprintf(NULL, 0, "unknown target triple '%s': %s", triple, msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "unknown target triple '%s': %s", triple, msg);
        *errorMessage = buf;

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
        "",
        "",
        LLVMCodeGenLevelDefault,
        LLVMRelocDefault,
        LLVMCodeModelDefault);

    if (defaultTriple)
    {
        LLVMDisposeMessage(defaultTriple);
    }

    if (!targetMachine)
    {
        *errorMessage = strdup("could not create target machine");

        return false;
    }

    LLVMSetTarget(bm->mod, triple);

    LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(targetMachine);
    char* dataLayoutStr = LLVMCopyStringRepOfTargetData(dataLayout);
    LLVMSetDataLayout(bm->mod, dataLayoutStr);
    LLVMDisposeMessage(dataLayoutStr);
    LLVMDisposeTargetData(dataLayout);

    LLVMCodeGenFileType kind = assembly ? LLVMAssemblyFile : LLVMObjectFile;
    char* emitError = NULL;
    bool ok = !LLVMTargetMachineEmitToFile(targetMachine, bm->mod, path, kind, &emitError);

    if (!ok)
    {
        const char* msg = emitError ? emitError : "(no message)";
        int needed = snprintf(NULL, 0, "emission failed: %s", msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "emission failed: %s", msg);
        *errorMessage = buf;
    }

    if (emitError)
    {
        LLVMDisposeMessage(emitError);
    }

    LLVMDisposeTargetMachine(targetMachine);

    return ok;
}

#endif
