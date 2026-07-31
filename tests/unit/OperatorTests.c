#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "ops", &err);
    if (err)
    {
        strataFree((char*)err);
    }
    return jit;
}

STRATA_TEST(jit_prefix_increment)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int x = 5;\n"
                                "  int y = ++x;\n"
                                "  return x * 10 + y;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 66);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_postfix_increment)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int x = 5;\n"
                                "  int y = x++;\n"
                                "  return x * 10 + y;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 65);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_prefix_decrement)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int x = 5;\n"
                                "  int y = --x;\n"
                                "  return x * 10 + y;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 44);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_postfix_decrement)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int x = 5;\n"
                                "  int y = x--;\n"
                                "  return x * 10 + y;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 45);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_increment_in_loop)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int sum = 0;\n"
                                "  for (int i = 0; i < 10; i++) {\n"
                                "    sum += i;\n"
                                "  }\n"
                                "  return sum;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 45);
        }
        strataJitDestroy(jit);
    }
}

static int g_sideEffectCount;

static int SideEffectTrue(void)
{
    g_sideEffectCount++;
    return 1;
}

static int SideEffectFalse(void)
{
    g_sideEffectCount++;
    return 0;
}

STRATA_TEST(jit_short_circuit_and_skips_rhs)
{
    g_sideEffectCount = 0;
    StrataJit* jit = CompileJit("extern int se_true();\n"
                                "extern int se_false();\n"
                                "int entry() {\n"
                                "  if (se_false() && se_true()) { return 1; }\n"
                                "  return 0;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "se_true", (void*)&SideEffectTrue);
        strataJitAddSymbol(jit, "se_false", (void*)&SideEffectFalse);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 0);
            STRATA_CHECK_EQ(g_sideEffectCount, 1);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_short_circuit_or_skips_rhs)
{
    g_sideEffectCount = 0;
    StrataJit* jit = CompileJit("extern int se_true();\n"
                                "extern int se_false();\n"
                                "int entry() {\n"
                                "  if (se_true() || se_false()) { return 1; }\n"
                                "  return 0;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "se_true", (void*)&SideEffectTrue);
        strataJitAddSymbol(jit, "se_false", (void*)&SideEffectFalse);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 1);
            STRATA_CHECK_EQ(g_sideEffectCount, 1);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_short_circuit_and_evaluates_both_when_lhs_true)
{
    g_sideEffectCount = 0;
    StrataJit* jit = CompileJit("extern int se_true();\n"
                                "extern int se_false();\n"
                                "int entry() {\n"
                                "  if (se_true() && se_false()) { return 1; }\n"
                                "  return 0;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "se_true", (void*)&SideEffectTrue);
        strataJitAddSymbol(jit, "se_false", (void*)&SideEffectFalse);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 0);
            STRATA_CHECK_EQ(g_sideEffectCount, 2);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_int_to_float)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int x = 7;\n"
                                "  float y = (float)x;\n"
                                "  return (int)(y * 2.0);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 14);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_float_to_int_truncates)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  float x = 3.9;\n"
                                "  return (int)x;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_double_to_float_to_int)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  double d = 2.5;\n"
                                "  float f = (float)d;\n"
                                "  return (int)(f * 4.0);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 10);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_uint_to_int)
{
    StrataJit* jit = CompileJit("uint entry() {\n"
                                "  int x = -1;\n"
                                "  return (uint)x;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        unsigned (*f)(void) = (unsigned (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK(f() == 0xFFFFFFFF);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_in_expression)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  float pi = 3.14159;\n"
                                "  int r = 5;\n"
                                "  return (int)(2 * (float)r * pi);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 31);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_scalar)
{
    StrataJit* jit = CompileJit("int g_counter = 42;\n"
                                "int entry() {\n"
                                "  return g_counter;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 42);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_mutable)
{
    StrataJit* jit = CompileJit("int g_val = 10;\n"
                                "int entry() {\n"
                                "  g_val += 5;\n"
                                "  return g_val;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 15);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_float_init)
{
    StrataJit* jit = CompileJit("float g_pi = 3.0;\n"
                                "float entry() {\n"
                                "  return g_pi * 2.0;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 5.9f && r < 6.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_zero_init)
{
    StrataJit* jit = CompileJit("int g_zero;\n"
                                "int entry() {\n"
                                "  return g_zero;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 0);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_shared_between_functions)
{
    StrataJit* jit = CompileJit("int g_count = 0;\n"
                                "void increment() {\n"
                                "  g_count++;\n"
                                "}\n"
                                "int entry() {\n"
                                "  increment();\n"
                                "  increment();\n"
                                "  increment();\n"
                                "  return g_count;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_negative_init)
{
    StrataJit* jit = CompileJit("int g_offset = -100;\n"
                                "int entry() {\n"
                                "  return g_offset + 200;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 100);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_long_type)
{
    StrataJit* jit = CompileJit("long entry() {\n"
                                "  long a = 3000000000;\n"
                                "  long b = 2000000000;\n"
                                "  return a + b;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        long long (*f)(void) = (long long (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 5000000000LL);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_ulong_large_values)
{
    StrataJit* jit = CompileJit("ulong entry() {\n"
                                "  ulong a = 5000000000;\n"
                                "  return a;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        unsigned long long (*f)(void) = (unsigned long long (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ((long long)f(), (long long)5000000000ULL);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_long_arithmetic)
{
    StrataJit* jit = CompileJit("long entry() {\n"
                                "  long x = 1000000;\n"
                                "  long y = x * x;\n"
                                "  return y;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        long long (*f)(void) = (long long (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 1000000000000LL);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_long_int_mixed)
{
    StrataJit* jit = CompileJit("long entry() {\n"
                                "  int small = 5;\n"
                                "  long big = 1000000000;\n"
                                "  return big + small;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        long long (*f)(void) = (long long (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 1000000005LL);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_int_to_long)
{
    StrataJit* jit = CompileJit("long entry() {\n"
                                "  int x = 2147483647;\n"
                                "  return (long)x * 2;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        long long (*f)(void) = (long long (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 4294967294LL);
        }
        strataJitDestroy(jit);
    }
}
