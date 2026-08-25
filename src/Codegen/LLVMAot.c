#include "LLVMAot.h"
#include "Codegen/LLVMCApi.h"

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

bool EmitNativeFile(BuiltModule* bm, const char* path, bool assembly, char** errorMessage, const char* targetTriple)
{
    if (!bm->mod)
    {
        *errorMessage = DupString("no module to emit");

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

    LLVMTargetMachineRef targetMachine = LLVMCreateTargetMachine(target, triple, "", "", LLVMCodeGenLevelDefault,
                                                                 LLVMRelocDefault, LLVMCodeModelDefault);

    if (defaultTriple)
    {
        LLVMDisposeMessage(defaultTriple);
    }

    if (!targetMachine)
    {
        *errorMessage = DupString("could not create target machine");

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

char* EmitNativeMemory(BuiltModule* bm, size_t* outSize, char** errorMessage)
{
    if (outSize)
    {
        *outSize = 0;
    }

    if (!bm->mod)
    {
        *errorMessage = DupString("no module to emit");
        return NULL;
    }

    EnsureTargetsInitialized();

    char* defaultTriple = LLVMGetDefaultTargetTriple();
    const char* triple = defaultTriple ? defaultTriple : "x86_64-pc-windows-gnu";

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

        return NULL;
    }

    LLVMTargetMachineRef targetMachine = LLVMCreateTargetMachine(target, triple, "", "", LLVMCodeGenLevelDefault,
                                                                 LLVMRelocDefault, LLVMCodeModelDefault);

    if (defaultTriple)
    {
        LLVMDisposeMessage(defaultTriple);
    }

    if (!targetMachine)
    {
        *errorMessage = DupString("could not create target machine");
        return NULL;
    }

    LLVMSetTarget(bm->mod, triple);

    LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(targetMachine);
    char* dataLayoutStr = LLVMCopyStringRepOfTargetData(dataLayout);
    LLVMSetDataLayout(bm->mod, dataLayoutStr);
    LLVMDisposeMessage(dataLayoutStr);
    LLVMDisposeTargetData(dataLayout);

    char* emitError = NULL;
    LLVMMemoryBufferRef buffer = NULL;

    bool ok = !LLVMTargetMachineEmitToMemoryBuffer(targetMachine, bm->mod, LLVMObjectFile, &emitError, &buffer);

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

    if (!ok)
    {
        return NULL;
    }

    const char* start = LLVMGetBufferStart(buffer);
    size_t size = LLVMGetBufferSize(buffer);
    char* bytes = (char*)malloc(size ? size : 1);

    if (!bytes)
    {
        LLVMDisposeMemoryBuffer(buffer);
        *errorMessage = DupString("out of memory copying emitted object");
        return NULL;
    }

    memcpy(bytes, start, size);
    LLVMDisposeMemoryBuffer(buffer);

    if (outSize)
    {
        *outSize = size;
    }

    return bytes;
}
