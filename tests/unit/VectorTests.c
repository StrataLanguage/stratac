#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#if STRATA_TEST_HAS_LLVM
#include "Codegen/LLVMModuleBuilder.h"
#endif

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
    run_int("int entry() {\n"
            "  float3 v = float3(1.0, 2.0, 3.0);\n"
            "  return (int)(v.x + v.y + v.z);\n" /* 6 */
            "}\n",
            6);
}

STRATA_TEST(vector_add_lanes)
{
    run_int("int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 b = float3(10.0, 20.0, 30.0);\n"
            "  float3 c = a + b;\n"
            "  return (int)(c.x + c.y + c.z);\n" /* 11 + 22 + 33 = 66 */
            "}\n",
            66);
}

STRATA_TEST(vector_sub_and_mul)
{
    run_int("int entry() {\n"
            "  float3 a = float3(10.0, 20.0, 30.0);\n"
            "  float3 b = float3(1.0, 2.0, 3.0);\n"
            "  float3 d = a - b;\n"                     /* {9, 18, 27} */
            "  float3 m = d * float3(2.0, 2.0, 2.0);\n" /* {18, 36, 54} */
            "  return (int)(m.x + m.y + m.z);\n"        /* 108 */
            "}\n",
            108);
}

STRATA_TEST(vector_splat)
{
    run_int("int entry() {\n"
            "  float3 s = float3(7.0);\n"        /* splat {7, 7, 7} */
            "  return (int)(s.x + s.y + s.z);\n" /* 21 */
            "}\n",
            21);
}

STRATA_TEST(vector_destructure_shrink)
{
    run_int("int entry() {\n"
            "  float4 a = float4(1.0, 2.0, 3.0, 4.0);\n"
            "  float2 b = a.zw;\n"
            "  float2 result = float2(1.0) + b;\n"
            "  return (int)(result.x + result.y);\n" /* 4.0 + 5.0 = 9 */
            "}\n",
            9);
}

STRATA_TEST(vector_swizzle_rebuilds)
{
    run_int("int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 sw = a.yyy;\n"                /* {2, 2, 2} */
            "  return (int)(sw.x + sw.y + sw.z);\n" /* 6 */
            "}\n",
            6);
}

STRATA_TEST(vector_passed_and_returned)
{
    run_int("float3 scale(float3 v, float3 s) {\n"
            "  return v * s;\n"
            "}\n"
            "int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 r = scale(a, float3(10.0, 10.0, 10.0));\n" /* {10, 20, 30} */
            "  return (int)(r.x + r.y + r.z);\n"                 /* 60 */
            "}\n",
            60);
}

STRATA_TEST(vector_binary_expr_as_call_arg)
{
    /* A binary vector expression passed directly as a call argument must
       infer the vector type (not fall through to the scalar `int` ladder)
       so overload resolution matches the float3 param. */
    run_int("float consume(float3 v)\n"
            "{\n"
            "  return (int)(v.x + v.y + v.z);\n"
            "}\n"
            "int entry()\n"
            "{\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  return consume(a * float3(10.0));\n" /* {10, 20, 30} */
            "}\n",
            60);
}

STRATA_TEST(vector_divide_lanes)
{
    run_int("int entry() {\n"
            "  float3 a = float3(10.0, 20.0, 30.0);\n"
            "  float3 b = float3(2.0, 2.0, 2.0);\n"
            "  float3 q = a / b;\n"              /* {5, 10, 15} */
            "  return (int)(q.x + q.y + q.z);\n" /* 30 */
            "}\n",
            30);
}

STRATA_TEST(vector_ref_rest_reassign_elements)
{
    /* ref float3... borrows the collected stack array mutably; each element
       can be reassigned through the ref and read back. */
    run_int("float3 bump(ref float3... vecs)\n"
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
            "  return (int)(r.x + r.y + r.z);\n" /* {114, 225, 336} -> 675 */
            "}\n",
            675);
}

STRATA_TEST(vector_ref_rest_aliases_sources)
{
    /* A ref float3... rest aliases the source variables: reassigning an
       element writes through to the caller's variable, and the mutation is
       visible in a later call with the same arguments. */
    run_int("void set_all(ref float3... vecs)\n"
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
            "  int s = (int)(a.x + a.y + a.z) + (int)(b.x + b.y + b.z);\n" /* 27 + 27 = 54 */
            "  set_all(a, b);\n"
            "  int t = (int)(a.x + a.y + a.z) + (int)(b.x + b.y + b.z);\n" /* 54 */
            "  return s + t;\n"                                            /* 108 */
            "}\n",
            108);
}

STRATA_TEST(vector_assign_lane_count_mismatch_is_error)
{
    /* Vector init/assignment requires an exact lane count. Truncations
       (float4 -> float2), widenings (float2 -> float4) and mixed-lane
       moves (float3 <-> float4) are rejected; only a scalar may splat into
       a vector. A non-vector never lands in a vector slot. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Vec3 { float x; float y; float z; };\n"
                    "int entry() {\n"
                    "  float2 a = float4(1.0, 2.0, 3.0, 4.0);\n"
                    "  float4 b = float2(1.0, 2.0);\n"
                    "  float3 c = float4(1.0, 2.0, 3.0, 4.0);\n"
                    "  float2 d = float3(1.0, 2.0, 3.0);\n"
                    "  float2 e = Vec3 { 1.0, 2.0, 3.0 };\n"
                    "  return 0;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "cannot be initialized") != NULL);
    STRATA_CHECK(strstr(d, "type 'float4'") != NULL);
    STRATA_CHECK(strstr(d, "type 'Vec3'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(vector_assign_same_lane_and_scalar_splat_ok)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  float2 a = float2(1.0, 2.0);\n"
                    "  float4 b = float4(1.0, 2.0, 3.0, 4.0);\n"
                    "  float3 c = float3(1.0, 2.0, 3.0);\n"
                    "  float2 d = 3.5;\n"
                    "  return 0;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(vector_dot3)
{
    /* dot(float3, float3) reduces to a scalar float: 1*4 + 2*5 + 3*6 = 32. */
    run_int("int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 b = float3(4.0, 5.0, 6.0);\n"
            "  float d = dot(a, b);\n"
            "  return (int)d;\n" /* 32 */
            "}\n",
            32);
}

STRATA_TEST(vector_cross3)
{
    /* cross(float3, float3) = (ay*bz-az*by, az*bx-ax*bz, ax*by-ay*bx)
       = (2*6-3*5, 3*4-1*6, 1*5-2*4) = (-3, 6, -3); sum = 0. */
    run_int("int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 b = float3(4.0, 5.0, 6.0);\n"
            "  float3 c = cross(a, b);\n"
            "  return (int)(c.x + c.y + c.z);\n" /* 0 */
            "}\n",
            0);
}

STRATA_TEST(vector_dot4)
{
    /* 1*4 + 2*5 + 3*6 + 4*7 = 60. */
    run_int("int entry() {\n"
            "  float4 a = float4(1.0, 2.0, 3.0, 4.0);\n"
            "  float4 b = float4(4.0, 5.0, 6.0, 7.0);\n"
            "  return (int)dot(a, b);\n" /* 60 */
            "}\n",
            60);
}

STRATA_TEST(vector_cross4)
{
    /* cross(float4, float4) treats lanes 0..2 as the 3-vector and zeroes w:
       y*bz - z*by etc. = (-3, 6, -3, 0); sum = 0. */
    run_int("int entry() {\n"
            "  float4 a = float4(1.0, 2.0, 3.0, 4.0);\n"
            "  float4 b = float4(4.0, 5.0, 6.0, 7.0);\n"
            "  float4 c = cross(a, b);\n"
            "  return (int)(c.x + c.y + c.z + c.w);\n" /* 0 */
            "}\n",
            0);
}

STRATA_TEST(vector_dot2)
{
    /* 1*3 + 2*4 = 11. */
    run_int("int entry() {\n"
            "  float2 a = float2(1.0, 2.0);\n"
            "  float2 b = float2(3.0, 4.0);\n"
            "  return (int)dot(a, b);\n" /* 11 */
            "}\n",
            11);
}

STRATA_TEST(vector_cross2_is_scalar_z)
{
    /* cross(float2, float2) returns the scalar z: 1*4 - 2*3 = -2. */
    run_int("int entry() {\n"
            "  float2 a = float2(1.0, 2.0);\n"
            "  float2 b = float2(3.0, 4.0);\n"
            "  float z = cross(a, b);\n"
            "  return (int)z;\n" /* -2 */
            "}\n",
            -2);
}

STRATA_TEST(vector_dot_cross_combined_checksum)
{
    /* Exercises dot/cross across float2/3/4 in one function:
       dot3=32, cross3=(−3,6,−3)->0, dot4=60, cross4=(-3,6,-3,0)->0,
       dot2=11, cross2=-2.  Total = 101. */
    run_int("int entry() {\n"
            "  float3 a = float3(1.0, 2.0, 3.0);\n"
            "  float3 b = float3(4.0, 5.0, 6.0);\n"
            "  float d = dot(a, b);\n"
            "  float3 c = cross(a, b);\n"
            "  float4 a4 = float4(1.0, 2.0, 3.0, 4.0);\n"
            "  float4 b4 = float4(4.0, 5.0, 6.0, 7.0);\n"
            "  float d4 = dot(a4, b4);\n"
            "  float4 c4 = cross(a4, b4);\n"
            "  float2 a2 = float2(1.0, 2.0);\n"
            "  float2 b2 = float2(3.0, 4.0);\n"
            "  float d2 = dot(a2, b2);\n"
            "  float z2 = cross(a2, b2);\n"
            "  int r = (int)(d + c.x + c.y + c.z + d4 + c4.x + c4.y + c4.z + d2 + z2);\n"
            "  return r;\n" /* 101 */
            "}\n",
            101);
}

STRATA_TEST(vector_dot_cross_lane_mismatch_is_error)
{
    /* Both operands of dot/cross must be SIMD vectors with matching lanes. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() {\n"
                    "  float3 a = float3(1.0, 2.0, 3.0);\n"
                    "  float3 b = float3(4.0, 5.0, 6.0);\n"
                    "  float x = dot(a, b);\n"                          /* ok */
                    "  float y = dot(a, float4(1.0, 2.0, 3.0, 4.0));\n" /* lane mismatch */
                    "  float z = cross(a, 1.0);\n"                      /* second arg not a vector */
                    "  return 0;\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "same lane count") != NULL);
    STRATA_CHECK(strstr(d, "SIMD vector arguments") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(vector_lane_writes_on_struct_field)
{
    /* Single-lane writes through a struct field: plain, compound, ++/--.
       (The exact repro from the GL demo issue: `s.pos.x = s.pos.x + ...`.) */
    run_int("struct Sprite { float2 pos; float2 vel; };\n"
            "void update(ref Sprite s, float dt)\n"
            "{\n"
            "  s.pos.x = s.pos.x + s.vel.x * dt;\n"
            "  s.pos.y += 1.0;\n"
            "  s.vel.y -= 0.5;\n"
            "  s.pos.x++;\n"
            "  --s.pos.y;\n"
            "}\n"
            "int entry()\n"
            "{\n"
            "  Sprite s = { .pos = float2(1.0, 2.0), .vel = float2(3.0, 4.0) };\n"
            "  update(s, 2.0);\n"
            "  return (int)(s.pos.x * 100.0 + s.pos.y * 10.0 + s.vel.y * 10.0);\n"
            "}\n",
            855);
}

STRATA_TEST(vector_lane_writes_on_local)
{
    run_int("int entry() {\n"
            "  float3 v = float3(1.0, 2.0, 3.0);\n"
            "  v.x = 10.0;\n"
            "  v.z += 30.0;\n"
            "  v.y /= 2.0;\n"
            "  v.x *= 2.0;\n"
            "  v.z -= 3.0;\n"
            "  v.y++;\n"
            "  return (int)(v.x + v.y + v.z);\n" /* 20 + 2 + 30 = 52 */
            "}\n",
            52);
}

STRATA_TEST(vector_lane_writes_on_boxed_vector)
{
    run_int("int entry() {\n"
            "  ^float3 v = float3(1.0, 2.0, 3.0);\n"
            "  v.x = 10.0;\n"
            "  v.z += 30.0;\n"
            "  float3 w = v;\n"
            "  return (int)(w.x + w.y + w.z);\n" /* 45 */
            "}\n",
            45);
}

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(vector_swizzle_write_is_error)
{
    /* Multi-component swizzle writes are rejected with a clear message. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Sprite { float2 pos; };\n"
                                  "int entry() {\n"
                                  "  Sprite s;\n"
                                  "  s.pos.xy = s.pos;\n"
                                  "  return 0;\n"
                                  "}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule llvmModule = BuildLlvmModule(mod, &diag, &arena, false, NULL);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "swizzle assignment is not supported") != NULL);

    BuiltModuleDispose(&llvmModule);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(vector_lane_out_of_range_write_is_error)
{
    /* `v.z` on a float2 has no lane 2. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("int entry() {\n"
                                  "  float2 v = float2(1.0, 2.0);\n"
                                  "  v.z = 3.0;\n"
                                  "  return 0;\n"
                                  "}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule llvmModule = BuildLlvmModule(mod, &diag, &arena, false, NULL);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "lane 'z' is out of range") != NULL);

    BuiltModuleDispose(&llvmModule);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
#endif
