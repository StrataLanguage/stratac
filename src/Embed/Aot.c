// Public AOT API: compile a module to an object file or to assembly.
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if STRATA_HAS_LLVM
    // Target triple for an explicit architecture: the host triple with its
    // arch component swapped, so vendor/OS/environment stay the host's
    // (e.g. x86_64-w64-windows-gnu -> aarch64-w64-windows-gnu). Returns NULL
    // (use the host default) for STRATA_ARCH_AUTO. Caller frees.
    static char* ArchTargetTriple(StrataArch arch)
    {
        const char* archName = NULL;

        switch (arch)
        {
        case STRATA_ARCH_X64:
            archName = "x86_64";
            break;
        case STRATA_ARCH_ARM64:
            archName = "aarch64";
            break;
        default:
            return NULL;
        }

        char* host = LLVMGetDefaultTargetTriple();
        const char* rest = host ? strchr(host, '-') : NULL;
        char* triple = ConcatOwned(archName, rest ? rest : "-unknown-unknown");

        if (host)
        {
            LLVMDisposeMessage(host);
        }

        return triple;
    }
#endif

    int strataCompileToObject(StrataCompiler* c, const char* inputPath, const char* outputPath, int assembly,
                              const char** errOut)
    {
        (void)c;

#if !STRATA_HAS_LLVM
        (void)inputPath;
        (void)outputPath;
        (void)assembly;

        SetErrOut(errOut, "LLVM backend not built", "");

        return 0;
#else

    if (!inputPath || !outputPath)
    {
        SetErrOut(errOut, "null input or output path", "");
        return 0;
    }

    Arena arena;
    DiagnosticEngine diag;
    ModuleLoader loader;
    InitModuleLoader(&arena, &diag, &loader, c);

    Module* mod = ModuleLoaderLoad(&loader, inputPath);
    AnalyzeModule(mod, &diag, &arena);

    if (DiagHasErrors(&diag) || !mod)
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);
        SetErrOut(errOut, diagText, "compilation failed");

        TeardownCompile(mod, &loader, &diag, &arena);

        return 0;
    }

    BuiltModule bm = BuildLlvmModuleEx(mod, &diag, &arena, false, NULL, c ? c->symbolPrefix : NULL);

    if (DiagHasErrors(&diag))
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);
        SetErrOut(errOut, diagText, "code generation failed");

        BuiltModuleDispose(&bm);
        TeardownCompile(mod, &loader, &diag, &arena);

        return 0;
    }

    char* emitErr = NULL;
    char* triple = ArchTargetTriple(c ? c->arch : STRATA_ARCH_AUTO);
    int ok = EmitNativeFile(&bm, outputPath, assembly, &emitErr, triple);

    if (!ok)
    {
        SetErrOut(errOut, emitErr, "emission failed");
    }

    free(triple);
    free(emitErr);

    BuiltModuleDispose(&bm);
    TeardownCompile(mod, &loader, &diag, &arena);

    return ok;
#endif
    }

#ifdef __cplusplus
}
#endif
