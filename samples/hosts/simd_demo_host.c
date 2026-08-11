// simd_demo_host.c — host side of simd.strata.
//
// Uses the public JIT API (strataJitCompileFile) to load and run the SIMD
// vector sample at runtime. The C backend lowers float3/float4 ops to a plain
// struct under the JIT, so no platform intrinsics are needed here.
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

/* Resolve a script `extern` to a host function. simd.strata declares printf. */
static void* resolve_extern(const char* name)
{
    if (strcmp(name, "printf") == 0)
    {
        return (void*)printf;
    }

    return NULL;
}

int main(void)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;

    char path[512];
    snprintf(path, sizeof path, "%s/simd.strata", STRATA_SAMPLE_DIR);
    StrataJit* jit = strataJitCompileFile(c, path, &err);

    if (!jit)
    {
        fprintf(stderr, "JIT compile failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    /* Discover what the script declared as `extern` and bind each one. */
    for (size_t i = 0; i < strataJitGetExternSymbolCount(jit); ++i)
    {
        const char* name = strataJitGetExternSymbolName(jit, i);
        void* fn = resolve_extern(name);

        if (!fn || !strataJitAddSymbol(jit, name, fn))
        {
            fprintf(stderr, "no host binding for extern '%s'\n", name);
        }
    }

    int (*run)(void) = (int (*)(void))strataJitGetFunction(jit, "run");

    if (run)
    {
        printf("run() = %d\n", run());   /* 87 */
    }
    else
    {
        fprintf(stderr, "could not resolve 'run'");
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}
