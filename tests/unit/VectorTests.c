#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>

static StrataJit* CompileVec(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "vec", err);
    strataCompilerDestroy(c);
    return jit;
}

static void run_int(const char* src, int expected)
{
    const char* err = NULL;
    StrataJit* jit = CompileVec(src, &err);

    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        STRATA_CHECK(false);
        return;
    }

    strataFree((char*)err);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), expected);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(vector_construct_and_read_lanes)
{
    run_int(
        "int entry() {\n"
        "  float3 v = float3(1.0, 2.0, 3.0);\n"
        "  return (int)(v.x + v.y + v.z);\n"     /* 6 */
        "}\n",
        6);
}

STRATA_TEST(vector_add_lanes)
{
    run_int(
        "int entry() {\n"
        "  float3 a = float3(1.0, 2.0, 3.0);\n"
        "  float3 b = float3(10.0, 20.0, 30.0);\n"
        "  float3 c = a + b;\n"
        "  return (int)(c.x + c.y + c.z);\n"     /* 11 + 22 + 33 = 66 */
        "}\n",
        66);
}

STRATA_TEST(vector_sub_and_mul)
{
    run_int(
        "int entry() {\n"
        "  float3 a = float3(10.0, 20.0, 30.0);\n"
        "  float3 b = float3(1.0, 2.0, 3.0);\n"
        "  float3 d = a - b;\n"                   /* {9, 18, 27} */
        "  float3 m = d * float3(2.0, 2.0, 2.0);\n" /* {18, 36, 54} */
        "  return (int)(m.x + m.y + m.z);\n"      /* 108 */
        "}\n",
        108);
}

STRATA_TEST(vector_splat)
{
    run_int(
        "int entry() {\n"
        "  float3 s = float3(7.0);\n"             /* splat {7, 7, 7} */
        "  return (int)(s.x + s.y + s.z);\n"      /* 21 */
        "}\n",
        21);
}

STRATA_TEST(vector_swizzle_rebuilds)
{
    run_int(
        "int entry() {\n"
        "  float3 a = float3(1.0, 2.0, 3.0);\n"
        "  float3 sw = a.yyy;\n"                  /* {2, 2, 2} */
        "  return (int)(sw.x + sw.y + sw.z);\n"   /* 6 */
        "}\n",
        6);
}

STRATA_TEST(vector_passed_and_returned)
{
    run_int(
        "float3 scale(float3 v, float3 s) {\n"
        "  return v * s;\n"
        "}\n"
        "int entry() {\n"
        "  float3 a = float3(1.0, 2.0, 3.0);\n"
        "  float3 r = scale(a, float3(10.0, 10.0, 10.0));\n" /* {10, 20, 30} */
        "  return (int)(r.x + r.y + r.z);\n"      /* 60 */
        "}\n",
        60);
}

STRATA_TEST(vector_float4_uses_four_lanes)
{
    run_int(
        "int entry() {\n"
        "  float4 v = float4(1.0, 2.0, 3.0, 4.0);\n"
        "  return (int)(v.x + v.y + v.z + v.w);\n" /* 10 */
        "}\n",
        10);
}

STRATA_TEST(vector_divide_lanes)
{
    run_int(
        "int entry() {\n"
        "  float3 a = float3(10.0, 20.0, 30.0);\n"
        "  float3 b = float3(2.0, 2.0, 2.0);\n"
        "  float3 q = a / b;\n"                    /* {5, 10, 15} */
        "  return (int)(q.x + q.y + q.z);\n"       /* 30 */
        "}\n",
        30);
}
