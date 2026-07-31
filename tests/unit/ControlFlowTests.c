#include "strata/Test.h"
#include "strata/strata.h"


static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "cfg", &err);
    if (err)
    {
        strataFree((char*)err);
    }
    return jit;
}

STRATA_TEST(jit_inout_param_writes_back)
{
    StrataJit* jit = CompileJit("void add_one(inout int x) { x = x + 1; }\n"
                                "int entry() { int n = 10; add_one(n); return n; }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 11);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_out_params_return_values)
{
    StrataJit* jit = CompileJit("void divmod(int a, int b, out int q, out int r) { q = a / b; r = a % b; }\n"
                                "int entry() {\n"
                                "  int q;\n"
                                "  int r;\n"
                                "  divmod(17, 5, q, r);\n"
                                "  return q * 100 + r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 302);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_if_else_branch)
{
    StrataJit* jit = CompileJit("int sign(int n) { if (n < 0) { return -1; } else { return 1; } }\n"
                                "int absval(int n) { int r = n; if (r < 0) { r = 0 - r; } return r; }\n"
                                "int entry() { return sign(-5) * 10 + sign(3) + absval(-7); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), -2);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_while_loop)
{
    StrataJit* jit = CompileJit("int sumto(int n) {\n"
                                "  int s = 0;\n"
                                "  int i = 1;\n"
                                "  while (i <= n) { s = s + i; i = i + 1; }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sumto(100); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 5050);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_break_and_continue)
{
    StrataJit* jit = CompileJit("int sumskip(int n) {\n"
                                "  int s = 0;\n"
                                "  int i = 0;\n"
                                "  while (true) {\n"
                                "    i = i + 1;\n"
                                "    if (i > n) { break; }\n"
                                "    if (i == 5) { continue; }\n"
                                "    s = s + i;\n"
                                "  }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sumskip(10); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 50);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_recursion)
{
    StrataJit* jit = CompileJit("int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
                                "int entry() { return fib(10); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 55);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_for_loop)
{
    StrataJit* jit = CompileJit("int factorial(int n) {\n"
                                "  int r = 1;\n"
                                "  for (int i = 1; i <= n; i = i + 1) { r = r * i; }\n"
                                "  return r;\n"
                                "}\n"
                                "int sum_squares(int n) {\n"
                                "  int s = 0;\n"
                                "  for (int i = 1; i <= n; i = i + 1) { s = s + i * i; }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return factorial(5) + sum_squares(4); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 150);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_for_loop_with_continue)
{
    StrataJit* jit = CompileJit("int sum_skip_evens(int n) {\n"
                                "  int s = 0;\n"
                                "  for (int i = 1; i <= n; i = i + 1) {\n"
                                "    if (i == 2 || i == 4) { continue; }\n"
                                "    s = s + i;\n"
                                "  }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sum_skip_evens(5); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 9);
        }
        strataJitDestroy(jit);
    }
}

