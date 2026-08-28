#include "Util.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"
#include "Codegen/CodegenBackend.h"
#include "Test.h"
#include "strata/strata.h"

#include <string.h>

STRATA_TEST(parser_type_alias)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Foo = uint;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 1);

    StructDecl* s = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(strcmp(s->name, "Foo") == 0);
    STRATA_CHECK(s->isTypeAlias);
    STRATA_CHECK(strcmp(s->underlyingType, "uint") == 0);
    STRATA_CHECK_EQ((long)s->fields.count, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_in_type_registry)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Meter = float;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "Meter"));
    STRATA_CHECK(strcmp(TypeRegistryGetUnderlyingType(&reg, "Meter"), "float") == 0);
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "Meter"), "float") == 0);
    STRATA_CHECK(!TypeRegistryIsTypeAlias(&reg, "float"));

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_chained_resolve)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct A = uint;\nstruct B = A;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "A"));
    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "B"));
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "B"), "uint") == 0);

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_scalar_like)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Meter = float;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    STRATA_CHECK(!IsScalarTypeName("Meter"));
    STRATA_CHECK(IsScalarLikeType(&reg, "Meter"));
    STRATA_CHECK(IsScalarLikeType(&reg, "float"));
    STRATA_CHECK(!IsScalarLikeType(&reg, "string"));

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_explicit_cast_both_ways)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Meter = float;\n"
        "Meter test() { return (Meter)3.14; }\n"
        "float test2() { Meter m = (Meter)1.0; return (float)m; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_no_implicit_conversion)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Meter = float;\n"
        "void test() { Meter m = 3.14; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_reject_struct_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Meter = float;\n"
        "void test() { Meter m = Meter { 1.0 }; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_cross_scalar_cast)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Meter = float;\n"
        "int test() { Meter m = (Meter)3.14; return (int)m; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_codegen_llvm_ir)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Meter = float;\n"
        "Meter test() { return (Meter)3.14; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define float @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_float2)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Dir = float2;\n"
        "Dir test() { float2 f2 = float2(1.0, 2.0); Dir d = (Dir)f2; return d; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define <2 x float> @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_float3)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 = float3;\n"
        "Vec3 test() { float3 f3 = float3(1.0, 2.0, 3.0); Vec3 v = (Vec3)f3; return v; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define <4 x float> @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_float4)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec4 = float4;\n"
        "Vec4 test() { float4 f4 = float4(1.0, 2.0, 3.0, 4.0); Vec4 v = (Vec4)f4; return v; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define <4 x float> @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_cast_between_aliases)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 = float3;\n"
        "struct Position = float3;\n"
        "Vec3 test() { float3 f3 = float3(1.0, 2.0, 3.0); Position p = (Position)f3; Vec3 v = (Vec3)p; return v; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define <4 x float> @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_no_implicit_conversion)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 = float3;\n"
        "void test() { float3 f3 = float3(1.0, 2.0, 3.0); Vec3 v = f3; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_cross_scalar_broadcast)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 = float3;\n"
        "void test() { float x = 5.0; Vec3 v = x; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_function_params)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 = float3;\n"
        "float dot_simd(Vec3 a, Vec3 b) { return dot(a, b); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define float @dot_simd") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_simd_return_type)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec4 = float4;\n"
        "Vec4 test(float x, float y, float z, float w) { float4 f4 = float4(x, y, z, w); return (Vec4)f4; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define <4 x float> @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
