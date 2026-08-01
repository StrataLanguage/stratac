#include "Test.h"
#include "strata/strata.h"

static int HostTriple(int value)
{
    return value * 3;
}

STRATA_TEST(tcc_jit_is_the_public_jit_backend)
{
    STRATA_CHECK((strataCapabilities() & STRATA_CAP_TCC_JIT) != 0);
    StrataCompiler* compiler = strataCompilerCreate();
    const char* error = NULL;
    StrataJit* jit = strataJitCompileString(
        compiler,
        "int square(int x) { return x * x; } int entry() { return square(9); }",
        "tcc_basic", &error);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 81);
        strataJitDestroy(jit);
    }
    strataFree((char*)error);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(tcc_jit_patches_extern_slots_after_relocation)
{
    StrataCompiler* compiler = strataCompilerCreate();
    const char* error = NULL;
    StrataJit* jit = strataJitCompileString(
        compiler,
        "extern int host_triple(int x); int entry() { return host_triple(14); }",
        "tcc_extern", &error);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ((long)strataJitGetExternSymbolCount(jit), 1);
        STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_triple", (void*)&HostTriple), 1);
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 42);
        strataJitDestroy(jit);
    }
    strataFree((char*)error);
    strataCompilerDestroy(compiler);
}
