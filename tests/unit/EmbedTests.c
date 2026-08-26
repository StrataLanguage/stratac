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
