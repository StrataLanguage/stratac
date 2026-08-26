// optional_demo_host.c -- host side of optional_demo.strata.
//
// Uses the public JIT API (strataJitCompileFile) to load and execute the
// optionals tour at runtime. The host provides `printf` so generated code
// can talk; strata_alloc / strata_free are wired by the runtime.
//
// Build:  cmake --build --preset default --target optional_demo
// Run:    build\default\bin\optional_demo.exe
#include <stdio.h>
#include <stdlib.h>

#include "strata/strata.h"

#ifndef STRATA_SAMPLE_DIR
#define STRATA_SAMPLE_DIR "."
#endif

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : STRATA_SAMPLE_DIR "/optional_demo.strata";

    printf("== Strata optionals demo ==\n");
    printf("loading %s\n\n", path);

    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;

    StrataProfile profile;
    profile.boundsCheck = 1;
    profile.nullExternCall = 1;

    strataJitSetBackend(c, STRATA_JIT_BACKEND_LLVM);
    strataJitSetProfile(c, &profile);

    StrataJit* jit = strataJitCompileFile(c, path, &err);

    if (!jit)
    {
        fprintf(stderr, "error: JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    strataJitAddSymbol(jit, "printf", (void*)&printf);

    int total = 0;

    const char* sections[] = {"DemoBasics", "DemoStrings", "DemoArrays", "DemoLinkedList", "DemoNested"};

    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); ++i)
    {
        int (*section)(void) = (int (*)(void))strataJitGetFunction(jit, sections[i]);

        if (!section)
        {
            fprintf(stderr, "error: section '%s' not found\n", sections[i]);
            strataJitDestroy(jit);
            strataCompilerDestroy(c);
            return 1;
        }

        int result = section();
        printf("  [%s] -> %d\n\n", sections[i], result);
        total += result;
    }

    printf("total checksum: %d (expect 109)\n", total);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);

    return total == 109 ? 0 : 1;
}
