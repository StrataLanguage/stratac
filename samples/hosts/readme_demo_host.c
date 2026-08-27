// readme_demo_host.c — host side of readme_demo.strata (the example in README.md).
//
// Binds `printf` and runs the example's `main()` via the public JIT API.
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

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
    snprintf(path, sizeof path, "%s/readme_demo.strata", STRATA_SAMPLE_DIR);
    StrataJit* jit = strataJitCompileFile(c, path, &err);

    if (!jit)
    {
        fprintf(stderr, "JIT compile failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    for (size_t i = 0; i < strataJitGetExternSymbolCount(jit); ++i)
    {
        const char* name = strataJitGetExternSymbolName(jit, i);
        void* fn = resolve_extern(name);

        if (!fn || !strataJitAddSymbol(jit, name, fn))
        {
            fprintf(stderr, "no host binding for extern '%s'\n", name);
        }
    }

    int (*main_fn)(void) = (int (*)(void))strataJitGetFunction(jit, "main");

    if (main_fn)
    {
        printf("main() = %d\n", main_fn());
    }
    else
    {
        fprintf(stderr, "could not resolve 'main'");
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}
