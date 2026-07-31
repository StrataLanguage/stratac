#include "Util.h"
#include "strata/Test.h"
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
