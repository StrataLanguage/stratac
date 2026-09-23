#include "Test.h"
#include "strata/strata.h"

#include <string.h>

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(embed_compile_string_ok)
{
    StrataCompiler* c = strataCompilerCreate();
    STRATA_CHECK(c != NULL);
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK_EQ(r.ok, 1);
    STRATA_CHECK_EQ(r.error_count, (unsigned)0);
    STRATA_CHECK(r.output != NULL);
    STRATA_CHECK(strstr(r.output, "define") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}
#endif

STRATA_TEST(embed_compile_string_reports_errors)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f( { }", "m", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK_EQ(r.ok, 0);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(r.diagnostics != NULL);
    STRATA_CHECK(strstr(r.diagnostics, "error") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_ast_emit)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_AST, 0);
    STRATA_CHECK_EQ(r.ok, 1);
    STRATA_CHECK(strstr(r.output, "fn int f") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_version_is_reported)
{
    const char* v = strataLLVMVersion();
    STRATA_CHECK(v != NULL);
    STRATA_CHECK(v[0] != '\0');
}

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(jit_explicit_llvm_backend_selection)
{
    /* Proves STRATA_JIT_BACKEND_LLVM works via strataJit* regardless of
       whether TCC is also compiled in (where AUTO would otherwise pick it). */
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetBackend(c, STRATA_JIT_BACKEND_LLVM);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, "int add(int a, int b) { return a + b; }", "explicit_llvm", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*add)(int, int) = (int (*)(int, int))strataJitGetFunction(jit, "add");
        STRATA_CHECK(add != NULL);
        if (add)
        {
            STRATA_CHECK_EQ(add(2, 3), 5);
        }

        STRATA_CHECK(strataJitCanInvokeIntVoid(jit, "add") == 0);
        strataJitDestroy(jit);
    }
    else
    {
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);
}
#endif

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(embed_result_counts_are_set)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK_EQ(r.ok, 1);
    STRATA_CHECK_EQ(r.error_count, (unsigned)0);
    STRATA_CHECK_EQ(r.warning_count, (unsigned)0);
    strataResultFree(&r);

    StrataResult bad = strataCompileString(c, "int f() { return x; }", "m", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK_EQ(bad.ok, 0);
    STRATA_CHECK_EQ(bad.error_count, (unsigned)1);
    STRATA_CHECK_EQ(bad.warning_count, (unsigned)0);
    strataResultFree(&bad);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_no_simd_flag_is_an_explicit_error)
{
    /* No backend can honor it yet, so it must not be silently ignored. */
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_LLVM_IR, STRATA_EMIT_NO_SIMD);
    STRATA_CHECK_EQ(r.ok, 0);
    STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "STRATA_EMIT_NO_SIMD") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_reserved_runtime_names_rejected)
{
    const char* sources[] = {
        "int strata_alloc() { return 1; }",
        "void strata_free(int x) { }",
        "int __strata_context_create() { return 0; }",
    };

    StrataCompiler* c = strataCompilerCreate();
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
    {
        StrataResult r = strataCompileString(c, sources[i], "m", STRATA_EMIT_LLVM_IR, 0);
        STRATA_CHECK_EQ(r.ok, 0);
        STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "reserved Strata runtime name") != NULL);
        strataResultFree(&r);
    }

    /* A name that merely resembles a runtime name is fine. */
    StrataResult ok = strataCompileString(c, "int strata_like() { return 1; }", "m", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK_EQ(ok.ok, 1);
    strataResultFree(&ok);
    strataCompilerDestroy(c);
}

/* Machine of an emitted object: COFF, ELF or Mach-O. 0 when unknown. */
static int ObjectMachine(const char* path)
{
    unsigned char h[20] = {0};
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);

    if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F')
    {
        int machine = h[18] | (h[19] << 8);
        return machine == 62 ? 1 : machine == 183 ? 2 : 0;
    }
    if (n >= 8 && h[0] == 0xcf && h[1] == 0xfa && h[2] == 0xed && h[3] == 0xfe)
    {
        long cpu = h[4] | (h[5] << 8) | (h[6] << 16) | ((long)h[7] << 24);
        return cpu == 0x01000007 ? 1 : cpu == 0x0100000c ? 2 : 0;
    }
    if (n >= 2)
    {
        int machine = h[0] | (h[1] << 8);
        return machine == 0x8664 ? 1 : machine == 0xaa64 ? 2 : 0;
    }
    return 0;
}

STRATA_TEST(embed_compile_to_object_honors_architecture)
{
    const char* tmp = getenv("TEMP");
    if (!tmp)
    {
        tmp = getenv("TMPDIR");
    }
    if (!tmp)
    {
        tmp = "/tmp";
    }

    char input[512];
    char output[512];
    snprintf(input, sizeof(input), "%s/hello.strata", STRATA_SAMPLE_DIR);

    const StrataArch arches[] = {STRATA_ARCH_X64, STRATA_ARCH_ARM64};
    const int expected[] = {1, 2};

    for (size_t i = 0; i < 2; i++)
    {
        snprintf(output, sizeof(output), "%s/strata_arch_test_%u.o", tmp, (unsigned)i);

        StrataCompiler* c = strataCompilerCreate();
        strataSetArchitecture(c, arches[i]);

        const char* err = NULL;
        int ok = strataCompileToObject(c, input, output, 0, &err);
        STRATA_CHECK(ok);
        if (!ok)
        {
            printf("  compile failed: %s\n", err ? err : "(none)");
        }
        strataFree((char*)err);

        STRATA_CHECK_EQ(ObjectMachine(output), expected[i]);
        remove(output);
        strataCompilerDestroy(c);
    }
}
#endif
