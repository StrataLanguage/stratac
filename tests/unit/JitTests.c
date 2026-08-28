#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>

#if STRATA_TEST_HAS_LLVM
#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"
#endif

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

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(aot_emits_native_object_file)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int add(int a, int b) { return a + b; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false, NULL);
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

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false, NULL);
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
#endif

static int HostDouble(int x)
{
    return x * 2;
}

static int HostAdd(int a, int b)
{
    return a + b;
}

typedef struct { int x; } HostFwd;

static void HostConsumeFwd(const HostFwd* f)
{
    (void)f;
}

STRATA_TEST(jit_extern_with_forward_declared_struct)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct Foo;\n"
        "extern void consume(const Foo f);\n"
        "struct Foo { int x; };\n"
        "int entry() { Foo f; f.x = 7; consume(f); return f.x; }\n",
        "fwdfwd", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "consume", (void*)&HostConsumeFwd), 1);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
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

static int g_customAllocs = 0;

static void* CustomAlloc(unsigned long long n)
{
    g_customAllocs++;
    return malloc((size_t)n);
}

static void CustomFree(void* p)
{
    free(p);
}

STRATA_TEST(jit_custom_allocator_is_used)
{
    /* The host can install a custom allocator; JIT code must call it. */
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, (void*)&CustomAlloc, (void*)&CustomFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int entry() { int[] a = {1, 2, 3}; int[] b = {10, 20}; array_push(a, 4); return a[0] + a[3] + (int)b.length; }",
        "custom_alloc", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 7);   /* 1 + 4 + 2 */
        }
        STRATA_CHECK(g_customAllocs > 0);
    }
    strataFree((char*)err);
    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_defer_in_lifo_order)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int g = 0;\n"
        "void run() {\n"
        "    defer g = g * 10 + 3;\n"
        "    defer g = g * 10 + 2;\n"
        "    g = g * 10 + 1;\n"
        "}\n"
        "int getg() { return g; }\n",
        "defer_lifo", &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    void (*run)(void) = (void (*)(void))strataJitGetFunction(jit, "run");
    int (*getg)(void) = (int (*)(void))strataJitGetFunction(jit, "getg");
    STRATA_CHECK(run != NULL);
    STRATA_CHECK(getg != NULL);

    if (run && getg)
    {
        run();
        /* body sets g=1, then LIFO defers: (+2) -> 12, (+3) -> 123 */
        STRATA_CHECK_EQ(getg(), 123);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_defer_on_early_return)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int g = 0;\n"
        "int run() {\n"
        "    defer g = g * 10 + 9;\n"
        "    g = g * 10 + 1;\n"
        "    if (g > 0) return g;\n"
        "    return 0;\n"
        "}\n"
        "int getg() { return g; }\n",
        "defer_ret", &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    int (*run)(void) = (int (*)(void))strataJitGetFunction(jit, "run");
    int (*getg)(void) = (int (*)(void))strataJitGetFunction(jit, "getg");
    STRATA_CHECK(run != NULL);
    STRATA_CHECK(getg != NULL);

    if (run && getg)
    {
        int r = run();
        STRATA_CHECK_EQ(r, 1);   /* return value snapshotted before defers run */
        STRATA_CHECK_EQ(getg(), 19); /* defer ran despite the early return */
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_defer_on_break_and_continue)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int gb = 0;\n"
        "int gc = 0;\n"
        "void run_break() {\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        defer gb = gb * 10 + i;\n"
        "        i = i + 1;\n"
        "        if (i == 2) break;\n"
        "    }\n"
        "}\n"
        "void run_continue() {\n"
        "    for (int i = 0; i < 3; i = i + 1) {\n"
        "        defer gc = gc * 10 + i;\n"
        "    }\n"
        "}\n"
        "int getgb() { return gb; }\n"
        "int getgc() { return gc; }\n",
        "defer_loop", &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    void (*run_break)(void) = (void (*)(void))strataJitGetFunction(jit, "run_break");
    void (*run_continue)(void) = (void (*)(void))strataJitGetFunction(jit, "run_continue");
    int (*getgb)(void) = (int (*)(void))strataJitGetFunction(jit, "getgb");
    int (*getgc)(void) = (int (*)(void))strataJitGetFunction(jit, "getgc");
    STRATA_CHECK(run_break != NULL);
    STRATA_CHECK(run_continue != NULL);
    STRATA_CHECK(getgb != NULL);
    STRATA_CHECK(getgc != NULL);

    if (run_break && getgb)
    {
        run_break();
        /* iter0: i becomes 1, defer runs (gb=0*10+1=1); iter1: i=2, break, defer runs (gb=1*10+2=12) */
        STRATA_CHECK_EQ(getgb(), 12);
    }
    if (run_continue && getgc)
    {
        run_continue();
        /* each iteration's defer runs: i=0->0, i=1->1, i=2->12 */
        STRATA_CHECK_EQ(getgc(), 12);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
