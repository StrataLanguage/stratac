// simd_demo_host.c — host side of simd.strata.
//
// Uses the public JIT API (strataJitCompileFile) to load and run the SIMD
// vector sample at runtime. The C backend lowers float3/float4 ops to a plain
// struct under the JIT, so no platform intrinsics are needed here.
#include "strata/strata.h"

#include <stdio.h>

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
