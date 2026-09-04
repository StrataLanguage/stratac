#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

static const char* ErrText(DiagnosticEngine* diag, Arena* arena)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < diag->m_count; i++)
    {
        SbPrintf(&sb, "%s; ", diag->m_diagnostics[i].message);
    }

    return SbFinish(&sb, arena);
}

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

/* Like CompileJit but keeps the error message for assertions. The compiler
 * intentionally outlives the returned jit (see TypeAliasTests.c usage). */
static StrataJit* CompileJitErr(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    *err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "ops", err);
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

STRATA_TEST(jit_cast_to_bool_compares_to_zero)
{
    /* (bool)x must be "x != 0", not bit-truncation: 2 and 4 have a zero low
       bit but are truthy. */
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int a = (bool)2;\n"
                                "  int b = (bool)4;\n"
                                "  int c = (bool)0;\n"
                                "  float f = 3.0;\n"
                                "  int d = (bool)f;\n"
                                "  return a + b + c + d;\n"   // 1 + 1 + 0 + 1 = 3
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

STRATA_TEST(jit_global_cast_literal_init)
{
    StrataJit* jit = CompileJit("struct ErrorCode = int;\n"
                                "ErrorCode g_err = (ErrorCode)404;\n"
                                "int g_plain = (int)7;\n"
                                "int entry() {\n"
                                "  return (int)g_err + g_plain;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 411);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_int_to_float_init)
{
    StrataJit* jit = CompileJit("float g_scale = 2;\n"
                                "int entry() {\n"
                                "  return (int)(g_scale * 2.5);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 5);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_global_float_to_int_init)
{
    StrataJit* jit = CompileJit("int g_trunc = (int)3.7;\n"
                                "int entry() {\n"
                                "  return g_trunc;\n"
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

STRATA_TEST(jit_global_bool_from_int_init_is_nonzero_test)
{
    /* `bool = 2` is a numeric-pair conversion: the value must be a (non)zero
     * test (true), NOT a bit-truncation of 2 into i1 (false). */
    StrataJit* jit = CompileJit("bool g_flag = 2;\n"
                                "int entry() {\n"
                                "  if (g_flag) { return 1; }\n"
                                "  return 0;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 1);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(global_init_undefined_ident_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileJitErr("int g = nope;\n"
                                   "int entry() { return 0; }\n",
                                   &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(global_init_nonconstant_expr_is_error)
{
    /* Sema accepts int = int, but the initializer must be a compile-time
     * constant - referencing another global is not foldable. */
    const char* err = NULL;
    StrataJit* jit = CompileJitErr("int g_a = 1;\n"
                                   "int g_b = g_a;\n"
                                   "int entry() { return g_b; }\n",
                                   &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(global_const_without_init_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileJitErr("const int g_max;\n"
                                   "int entry() { return g_max; }\n",
                                   &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
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

STRATA_TEST(jit_byte_type)
{
    StrataJit* jit = CompileJit("byte entry() {\n"
                                "  byte a = 200;\n"
                                "  byte b = 100;\n"
                                "  return a + b;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        unsigned char (*f)(void) = (unsigned char (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), (unsigned char)44);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_sbyte_type)
{
    StrataJit* jit = CompileJit("sbyte entry() {\n"
                                "  sbyte a = 100;\n"
                                "  sbyte b = 50;\n"
                                "  return a + b;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        signed char (*f)(void) = (signed char (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), (signed char)-106);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_short_type)
{
    StrataJit* jit = CompileJit("short entry() {\n"
                                "  short a = 30000;\n"
                                "  short b = 10000;\n"
                                "  return a + b;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        short (*f)(void) = (short (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), (short)-25536);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_ushort_type)
{
    StrataJit* jit = CompileJit("ushort entry() {\n"
                                "  ushort a = 60000;\n"
                                "  ushort b = 10000;\n"
                                "  return a + b;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        unsigned short (*f)(void) = (unsigned short (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), (unsigned short)4464);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_cast_int_to_byte)
{
    StrataJit* jit = CompileJit("byte entry() {\n"
                                "  int x = 300;\n"
                                "  return (byte)x;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        unsigned char (*f)(void) = (unsigned char (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), (unsigned char)44);
        }
        strataJitDestroy(jit);
    }
}

static int handle_get_int(void* e) { return (int)(intptr_t)e; }
static void* handle_spawn_1234(void) { return (void*)(intptr_t)0x1234; }
static void* handle_spawn_42(void) { return (void*)(intptr_t)42; }
static void* handle_spawn_99(void) { return (void*)(intptr_t)99; }
static void* handle_spawn_777(void) { return (void*)(intptr_t)777; }

STRATA_TEST(jit_handle_extends_pass_as_base)
{
    StrataJit* jit = CompileJit("handle Entity;\n"
                                "handle Player extends Entity;\n"
                                "extern int get_id(Entity e);\n"
                                "extern Player make_player();\n"
                                "int entry() {\n"
                                "  Player p = make_player();\n"
                                "  return get_id(p);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "get_id", (void*)&handle_get_int);
        strataJitAddSymbol(jit, "make_player", (void*)&handle_spawn_1234);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 0x1234);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_handle_extends_assign_to_base)
{
    StrataJit* jit = CompileJit("handle Entity;\n"
                                "handle Player extends Entity;\n"
                                "extern Player spawn();\n"
                                "extern int get_id(Entity e);\n"
                                "int entry() {\n"
                                "  Player p = spawn();\n"
                                "  Entity e = p;\n"
                                "  return get_id(e);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "spawn", (void*)&handle_spawn_42);
        strataJitAddSymbol(jit, "get_id", (void*)&handle_get_int);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 42);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_handle_extends_cast_down)
{
    StrataJit* jit = CompileJit("handle Entity;\n"
                                "handle Player extends Entity;\n"
                                "extern Entity spawn();\n"
                                "extern int get_player_id(Player p);\n"
                                "int entry() {\n"
                                "  Entity e = spawn();\n"
                                "  Player p = (Player)e;\n"
                                "  return get_player_id(p);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "spawn", (void*)&handle_spawn_99);
        strataJitAddSymbol(jit, "get_player_id", (void*)&handle_get_int);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 99);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_handle_extends_multi_level)
{
    StrataJit* jit = CompileJit("handle Entity;\n"
                                "handle Character extends Entity;\n"
                                "handle Player extends Character;\n"
                                "extern int get_id(Entity e);\n"
                                "extern Player create_player();\n"
                                "int entry() {\n"
                                "  Player p = create_player();\n"
                                "  return get_id(p);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        strataJitAddSymbol(jit, "get_id", (void*)&handle_get_int);
        strataJitAddSymbol(jit, "create_player", (void*)&handle_spawn_777);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 777);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_float_remainder)
{
    StrataJit* jit = CompileJit(
        "float rem(float x, float y) { return x % y; }\n"
        "int entry() { float x = 7.5; x %= 2.0; return (int)(x * 10.0 + rem(5.5, 2.0)); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 16);
        strataJitDestroy(jit);
    }
}

/* `int.max` etc. — the scalar pseudo-property. Integer scalars get `max`
   (the C limit macros: INT_MAX, UINT_MAX, ...); values are typed as the
   base scalar itself. */
STRATA_TEST(jit_scalar_max_integer_constants)
{
    StrataJit* jit = CompileJit(
        "int entry() {\n"
        "  int a = int.max;\n"
        "  uint b = uint.max;\n"
        "  long c = long.max;\n"
        "  ulong d = ulong.max;\n"
        "  byte e = byte.max;\n"
        "  sbyte f = sbyte.max;\n"
        "  short g = short.max;\n"
        "  ushort h = ushort.max;\n"
        "  return (int)(a == 2147483647) + (int)(b == 4294967295u) + (int)(c == 9223372036854775807)\n"
        "       + (int)(d == 18446744073709551615u) + (int)(e == 255) + (int)(f == 127)\n"
        "       + (int)(g == 32767) + (int)(h == 65535);\n"
        "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 8);
        strataJitDestroy(jit);
    }
}

/* Floats get `max` AND `min` (FLT_MAX/FLT_MIN, DBL_MAX/DBL_MIN). The
   double values are verified by round-trip + magnitude against real
   doubles built from int casts (float-derived doubles are limited to the
   float range by the literal typing). */
STRATA_TEST(jit_scalar_max_min_float_double)
{
    StrataJit* jit = CompileJit(
        "int entry() {\n"
        "  float fmax = float.max;\n"
        "  float fmin = float.min;\n"
        "  double dmax = double.max;\n"
        "  double dmin = double.min;\n"
        "  int r = 0;\n"
        "  if (fmax > 3.0e38f) { r = r + 1; }\n"          /* FLT_MAX 3.40e38 */
        "  if (fmin < 1.2e-38f) { r = r + 1; }\n"         /* FLT_MIN 1.17e-38 */
        "  if (dmax == double.max) { r = r + 1; }\n"      /* DBL_MAX 1.79e308 */
        "  if (dmin == double.min) { r = r + 1; }\n"      /* DBL_MIN 2.2e-308 */
        "  if (dmax > (double)9000000000) { r = r + 1; }\n" /* far above the float range */
        "  if (dmin < (double)1) { r = r + 1; }\n"          /* far below */
        "  return r;\n"
        "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 6);
        strataJitDestroy(jit);
    }
}

/* Pseudo-properties are real typed constants: they participate in
   arithmetic (including 64-bit wraparound) like any literal. */
STRATA_TEST(jit_scalar_max_in_arithmetic)
{
    StrataJit* jit = CompileJit(
        "int entry() {\n"
        "  int a = int.max - 2147483647;      /* 0 */\n"
        "  int b = (int)(ulong.max + 1 == 0); /* wraps to 0 */\n"
        "  return a + b;\n"
        "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 1);
        strataJitDestroy(jit);
    }
}

/* `const` manifest globals fold the pseudo-property (sema + codegen). */
STRATA_TEST(jit_scalar_max_in_const_global)
{
    StrataJit* jit = CompileJit(
        "const ulong G = ulong.max;\n"
        "const byte B = byte.max;\n"
        "int entry() { return (int)(G == ulong.max) + (int)(B == 255); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 2);
        strataJitDestroy(jit);
    }
}

/* Plain (non-const) global initializers fold too. */
STRATA_TEST(jit_scalar_max_in_global_init)
{
    StrataJit* jit = CompileJit(
        "ulong g = ulong.max;\n"
        "int entry() { return (int)(g == ulong.max); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 1);
        strataJitDestroy(jit);
    }
}

/* Enum member values may reference a scalar pseudo-property. */
STRATA_TEST(jit_scalar_max_in_enum_value)
{
    StrataJit* jit = CompileJit(
        "enum E : ulong { A = ulong.max };\n"
        "int entry() { ulong v = (ulong)E.A; return (int)(v == ulong.max); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry) STRATA_CHECK_EQ(entry(), 1);
        strataJitDestroy(jit);
    }
}

/* `min` is float-only: `int.min` is not a member. */
STRATA_TEST(scalar_max_int_min_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  int x = int.min;\n"
                    "  return x;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "type 'int' has no member 'min'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* Unknown members on a scalar type name are diagnosed directly. */
STRATA_TEST(scalar_max_unknown_member_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  int x = float.foo;\n"
                    "  return x;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "type 'float' has no member 'foo'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* Type aliases carry no pseudo-properties: `Meter.max` is left off. */
STRATA_TEST(scalar_max_alias_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Meter = int;\n"
                    "int entry() {\n"
                    "  Meter m = Meter.max;\n"
                    "  return (int)m;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* A value base is not a pseudo-property read: `x.max` has no member
   (diagnosed by the backend like any other unknown member). */
STRATA_TEST(scalar_max_value_base_is_an_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileJitErr("int entry() {\n"
                                   "  int x = 5;\n"
                                   "  int y = x.max;\n"
                                   "  return y;\n"
                                   "}\n",
                                   &err);
    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err && Contains(err, "cannot access member 'max'"));
    if (err) strataFree((char*)err);
}

/* Pseudo-properties are read-only constants. */
STRATA_TEST(scalar_max_assign_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  int x = (int.max = 5);\n"
                    "  return x;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "cannot assign to 'int.max' (a constant)"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scalar_max_compound_assign_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  int x = 0;\n"
                    "  x = (int.max += 1);\n"
                    "  return x;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "cannot assign to 'int.max' (a constant)"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scalar_max_incdec_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  int x = (int.max++);\n"
                    "  return x;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "cannot increment 'int.max' (a constant)"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
