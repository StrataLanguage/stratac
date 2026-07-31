#include "Util.h"
#include "strata/Test.h"
#include "strata/strata.h"

#include <stdio.h>

#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"

STRATA_TEST(jit_runs_int_addition)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, "int add(int a, int b) { return a + b; }", "math", &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    STRATA_CHECK(jit != NULL);

    int (*add)(int, int) = (int (*)(int, int))strataJitGetFunction(jit, "add");
    STRATA_CHECK(add != NULL);
    if (add)
    {
        STRATA_CHECK_EQ(add(2, 3), 5);
        STRATA_CHECK_EQ(add(-1, 1), 0);
        STRATA_CHECK_EQ(add(100, 23), 123);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_noarg_function_and_calls)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int sq(int x) { return x * x; }\n"
        "int answer() { return sq(7); }\n",
        "calls", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*answer)(void) = (int (*)(void))strataJitGetFunction(jit, "answer");
        STRATA_CHECK(answer != NULL);
        if (answer)
        {
            STRATA_CHECK_EQ(answer(), 49);
        }

        void* missing = strataJitGetFunction(jit, "does_not_exist");
        STRATA_CHECK(missing == NULL);

        strataJitDestroy(jit);
    }
    else
    {
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_float_function)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, "float twice(float x) { return x * 2.0; }", "flt", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*twice)(float) = (float (*)(float))strataJitGetFunction(jit, "twice");
        STRATA_CHECK(twice != NULL);
        if (twice)
        {
            float r = twice(21.0f);
            STRATA_CHECK(r > 41.999f && r < 42.001f);
        }

        strataJitDestroy(jit);
    }
    else
    {
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);
}

STRATA_TEST(aot_emits_native_object_file)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int add(int a, int b) { return a + b; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false);
    const char* path = "strata_aot_test.o";
    char* err = NULL;
    bool ok = EmitNativeFile(&bm, path, false, &err, NULL);
    if (!ok)
    {
        printf("  AOT emission failed: %s\n", err ? err : "(no message)");
    }

    STRATA_CHECK(ok);

    FILE* in = fopen(path, "rb");
    STRATA_CHECK(in != NULL);
    if (in)
    {
        fseek(in, 0, SEEK_END);
        STRATA_CHECK(ftell(in) > 0);
        fclose(in);
    }

    BuiltModuleDispose(&bm);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(aot_emits_assembly_file)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int forty_two() { return 42; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false);
    const char* path = "strata_aot_test.s";
    char* err = NULL;
    bool ok = EmitNativeFile(&bm, path, true, &err, NULL);
    if (!ok)
    {
        printf("  AOT asm emission failed: %s\n", err ? err : "(no message)");
    }

    STRATA_CHECK(ok);

    FILE* in = fopen(path, "rb");
    STRATA_CHECK(in != NULL);
    if (in)
    {
        fseek(in, 0, SEEK_END);
        STRATA_CHECK(ftell(in) > 0);
        fclose(in);
    }

    BuiltModuleDispose(&bm);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

static int HostDouble(int x)
{
    return x * 2;
}

static int HostAdd(int a, int b)
{
    return a + b;
}

STRATA_TEST(jit_calls_host_extern_function)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "extern int host_double(int x);\n"
        "extern int host_add(int a, int b);\n"
        "int entry(int x) { return host_add(host_double(x), 1); }\n",
        "ext", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK_EQ((long)strataJitGetExternSymbolCount(jit), 2);

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_double", (void*)&HostDouble), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_add", (void*)&HostAdd), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "not_declared", (void*)&HostAdd), 0);

    int (*entry)(int) = (int (*)(int))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(5), 11);
        STRATA_CHECK_EQ(entry(0), 1);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

