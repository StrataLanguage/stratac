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


STRATA_TEST(vector_ref_rest_reassign_elements)
{
    /* ref float3... borrows the collected stack array mutably; each element
       can be reassigned through the ref and read back. */
    run_int(
        "float3 bump(ref float3... vecs)\n"
        "{\n"
        "  for (uint i = 0; i < vecs.length; i++)\n"
        "  {\n"
        "    vecs[i] = vecs[i] + float3(1.0, 1.0, 1.0);\n"
        "  }\n"
        "  float3 total = float3(0.0, 0.0, 0.0);\n"
        "  for (uint i = 0; i < vecs.length; i++)\n"
        "  {\n"
        "    total = total + vecs[i];\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "int entry()\n"
        "{\n"
        "  float3 r = bump(float3(1.0, 2.0, 3.0), float3(10.0, 20.0, 30.0), float3(100.0, 200.0, 300.0));\n"
        "  return (int)(r.x + r.y + r.z);\n"       /* {114, 225, 336} -> 675 */
        "}\n",
        675);
}

STRATA_TEST(vector_ref_rest_aliases_sources)
{
    /* A ref float3... rest aliases the source variables: reassigning an
       element writes through to the caller's variable, and the mutation is
       visible in a later call with the same arguments. */
    run_int(
        "void set_all(ref float3... vecs)\n"
        "{\n"
        "  for (uint i = 0; i < vecs.length; i++)\n"
        "  {\n"
        "    vecs[i] = float3(9.0, 9.0, 9.0);\n"
        "  }\n"
        "}\n"
        "int entry()\n"
        "{\n"
        "  float3 a = float3(1.0, 2.0, 3.0);\n"
        "  float3 b = float3(10.0, 20.0, 30.0);\n"
        "  set_all(a, b);\n"
        "  int s = (int)(a.x + a.y + a.z) + (int)(b.x + b.y + b.z);\n"   /* 27 + 27 = 54 */
        "  set_all(a, b);\n"
        "  int t = (int)(a.x + a.y + a.z) + (int)(b.x + b.y + b.z);\n"   /* 54 */
        "  return s + t;\n"                                               /* 108 */
        "}\n",
        108);
}

