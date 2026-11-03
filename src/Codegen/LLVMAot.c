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

/* STRATA_ASAN=1: marks every defined function `sanitize_address` and runs LLVM's AddressSanitizer over the module,
   so the program links against clang's ASan runtime and reports use after free, overflow and double free in the
   generated code (roadmap phase 5 gate). The variable's value, when it is not "1", is appended to the names of the
   globals made external, so objects built from one source with different tags still link together. */
static void InstrumentAddressSanitizer(LLVMModuleRef mod, LLVMTargetMachineRef tm)
{
    /* The Windows runtime places the shadow memory itself; the code must load its address, not assume one. */
    const char* args[] = {"strata", "-asan-force-dynamic-shadow"};
    LLVMParseCommandLineOptions(2, args, NULL);

    /* The pass gives each instrumented global an associated COMDAT, which COFF cannot attach to a local symbol.
       Making the globals external for the instrumented build avoids that; nothing links against them. */
    for (LLVMValueRef g = LLVMGetFirstGlobal(mod); g; g = LLVMGetNextGlobal(g))
    {
        int linkage = LLVMGetLinkage(g);

        if (linkage == LLVMInternalLinkage || linkage == LLVMPrivateLinkage)
        {
            LLVMSetLinkage(g, LLVMExternalLinkage);

            const char* tag = getenv("STRATA_ASAN");
            size_t length = 0;
            const char* name = LLVMGetValueName2(g, &length);

            if (tag && strcmp(tag, "1") != 0 && length > 0)
            {
                char renamed[512];

                snprintf(renamed, sizeof renamed, "%.*s.%s", (int)length, name, tag);
                LLVMSetValueName2(g, renamed, strlen(renamed));
            }
        }
    }

    LLVMContextRef ctx = LLVMGetModuleContext(mod);
    unsigned kind = LLVMGetEnumAttributeKindForName("sanitize_address", 16);
    LLVMAttributeRef attr = LLVMCreateEnumAttribute(ctx, kind, 0);

    for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn; fn = LLVMGetNextFunction(fn))
    {
        if (LLVMCountBasicBlocks(fn) > 0)
        {
            LLVMAddAttributeAtIndex(fn, (unsigned)-1, attr); /* LLVMAttributeFunctionIndex */
        }
    }

    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef err = LLVMRunPasses(mod, "asan", tm, options);

    if (err)
    {
        LLVMConsumeError(err);
    }

    LLVMDisposePassBuilderOptions(options);
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

    if (getenv("STRATA_ASAN"))
    {
        InstrumentAddressSanitizer(bm->mod, targetMachine);
    }

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
