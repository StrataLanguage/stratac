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

/* ── Pseudo-enum tests: multiple int-based aliases ─────────────────────── */

STRATA_TEST(pseudo_enum_multiple_int_aliases_compile)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "struct Channel = int;\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "ErrorCode"));
    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "Priority"));
    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "Channel"));
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "ErrorCode"), "int") == 0);
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "Priority"), "int") == 0);
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "Channel"), "int") == 0);

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_no_implicit_cross_cast)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "void test() { ErrorCode e = (ErrorCode)42; Priority p = e; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_no_implicit_from_int)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "void test() { ErrorCode e = 42; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_no_implicit_to_int)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "void test() { ErrorCode e = (ErrorCode)42; int x = e; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_explicit_cast_int_to_alias)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "ErrorCode test() { return (ErrorCode)42; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_explicit_cast_alias_to_int)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "int test() { ErrorCode e = (ErrorCode)42; return (int)e; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_explicit_cross_alias_cast)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "Priority test() { ErrorCode e = (ErrorCode)42; return (Priority)e; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @test") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_cast_through_underlying)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "Priority test() { ErrorCode e = (ErrorCode)7; int raw = (int)e; return (Priority)raw; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_function_params)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "ErrorCode classify(ErrorCode e) { return e; }\n"
        "Priority lift(ErrorCode e) { return (Priority)e; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @classify") != NULL);
    STRATA_CHECK(strstr(res.output, "define i32 @lift") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_arithmetic_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "void test() { ErrorCode a = (ErrorCode)1; Priority b = (Priority)2; void* c = a + b; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_comparison_allowed_same_underlying)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "bool test() { ErrorCode a = (ErrorCode)1; Priority b = (Priority)2; return a == b; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_comparison_different_underlying_allowed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Rate = float;\n"
        "bool test() { ErrorCode a = (ErrorCode)1; Rate b = (Rate)2.0; return a == b; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_chained_alias)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct HttpError = ErrorCode;\n"
        "HttpError test() { return (HttpError)404; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    STRATA_CHECK(TypeRegistryIsTypeAlias(&reg, "HttpError"));
    STRATA_CHECK(strcmp(TypeRegistryResolveAlias(&reg, "HttpError"), "int") == 0);

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @test") != NULL);

    free((void*)res.output);
    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_chained_alias_no_implicit_mix)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct HttpError = ErrorCode;\n"
        "struct Priority = int;\n"
        "void test() { HttpError e = (HttpError)404; Priority p = e; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ── Pseudo-enum: module-level constants ───────────────────────────────── */

STRATA_TEST(pseudo_enum_global_def_int_alias)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "ErrorCode Opaque = (ErrorCode)0;\n"
        "ErrorCode Not_Found = (ErrorCode)404;\n"
        "ErrorCode Server_Error = (ErrorCode)500;\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK_EQ((long)mod->globals.count, 3);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_def_uint_alias)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct RenderBucket = uint;\n"
        "RenderBucket Opaque = (RenderBucket)0;\n"
        "RenderBucket Translucent = (RenderBucket)1;\n"
        "RenderBucket Additive = (RenderBucket)2;\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK_EQ((long)mod->globals.count, 3);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_def_mixed_aliases)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "ErrorCode Ok = (ErrorCode)200;\n"
        "ErrorCode Not_Found = (ErrorCode)404;\n"
        "Priority Low = (Priority)0;\n"
        "Priority High = (Priority)10;\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK_EQ((long)mod->globals.count, 4);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_used_in_function)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "ErrorCode Ok = (ErrorCode)200;\n"
        "ErrorCode Not_Found = (ErrorCode)404;\n"
        "ErrorCode classify(int raw) { return (ErrorCode)raw; }\n"
        "bool is_success(ErrorCode e) { return (int)e == (int)Ok; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_cross_type_init_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "Priority Bad = (ErrorCode)42;\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_raw_int_init_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct ErrorCode = int;\n"
        "ErrorCode Bad = 42;\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_global_codegen_ir)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct RenderBucket = uint;\n"
        "RenderBucket Opaque = (RenderBucket)0;\n"
        "RenderBucket Translucent = (RenderBucket)1;\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "@Opaque") != NULL);
    STRATA_CHECK(strstr(res.output, "@Translucent") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(pseudo_enum_jit_global_def)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "ErrorCode Opaque = (ErrorCode)0;\n"
        "ErrorCode Not_Found = (ErrorCode)404;\n"
        "ErrorCode Server_Error = (ErrorCode)500;\n"
        "int get_value(ErrorCode e) { return (int)e; }\n"
        "int caller() { return get_value(Not_Found); }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*caller)(void) = (int (*)(void))strataJitGetFunction(jit, "caller");
        STRATA_CHECK(caller != NULL);
        if (caller)
        {
            STRATA_CHECK_EQ(caller(), 404);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(pseudo_enum_jit_global_switch)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct RenderBucket = uint;\n"
        "RenderBucket Opaque = (RenderBucket)0;\n"
        "RenderBucket Translucent = (RenderBucket)1;\n"
        "RenderBucket Additive = (RenderBucket)2;\n"
        "RenderBucket current() { return Translucent; }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*current)(void) = (int (*)(void))strataJitGetFunction(jit, "current");
        STRATA_CHECK(current != NULL);
        if (current)
        {
            STRATA_CHECK_EQ(current(), 1);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

/* ── JIT runtime tests: verify casts produce correct values ───────────── */

STRATA_TEST(pseudo_enum_jit_cast_int_to_alias)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "ErrorCode make_error() { return (ErrorCode)42; }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*make_error)(void) = (int (*)(void))strataJitGetFunction(jit, "make_error");
        STRATA_CHECK(make_error != NULL);
        if (make_error)
        {
            STRATA_CHECK_EQ(make_error(), 42);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(pseudo_enum_jit_cast_alias_to_int)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "int unwrap(ErrorCode e) { return (int)e; }\n"
        "int caller() { ErrorCode e = (ErrorCode)99; return unwrap(e); }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*caller)(void) = (int (*)(void))strataJitGetFunction(jit, "caller");
        STRATA_CHECK(caller != NULL);
        if (caller)
        {
            STRATA_CHECK_EQ(caller(), 99);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(pseudo_enum_jit_cross_alias_cast)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "Priority promote(ErrorCode e) { return (Priority)e; }\n"
        "int caller() { ErrorCode e = (ErrorCode)7; return (int)promote(e); }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*caller)(void) = (int (*)(void))strataJitGetFunction(jit, "caller");
        STRATA_CHECK(caller != NULL);
        if (caller)
        {
            STRATA_CHECK_EQ(caller(), 7);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(pseudo_enum_jit_chained_cast)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "struct HttpError = ErrorCode;\n"
        "struct Priority = int;\n"
        "int caller() { HttpError e = (HttpError)404; Priority p = (Priority)e; return (int)p; }\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*caller)(void) = (int (*)(void))strataJitGetFunction(jit, "caller");
        STRATA_CHECK(caller != NULL);
        if (caller)
        {
            STRATA_CHECK_EQ(caller(), 404);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(pseudo_enum_jit_arithmetic_through_casts)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct ErrorCode = int;\n"
        "struct Priority = int;\n"
        "int combine() {\n"
        "    ErrorCode a = (ErrorCode)10;\n"
        "    Priority b = (Priority)20;\n"
        "    int raw_a = (int)a;\n"
        "    int raw_b = (int)b;\n"
        "    return raw_a + raw_b;\n"
        "}\n",
        "pseudo_enum", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*combine)(void) = (int (*)(void))strataJitGetFunction(jit, "combine");
        STRATA_CHECK(combine != NULL);
        if (combine)
        {
            STRATA_CHECK_EQ(combine(), 30);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}

/* ── Strong newtype over string (owning underlying) ─────────────────────── */

STRATA_TEST(type_alias_string_explicit_cast_both_ways)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "Name make() { return (Name)\"ada\"; }\n"
        "string take(Name n) { return (string)n; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_no_implicit_conversion)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "void test() { Name n = \"ada\"; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_no_implicit_back)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "void test() { Name n = (Name)\"ada\"; string s = n; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_cross_alias_no_implicit)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "struct Id = string;\n"
        "void test() { Name n = (Name)\"a\"; Id i = n; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_local_requires_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "void test() { Name n; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_global_requires_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "Name g;\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_global_literal_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "Name g = (Name)\"owned\";\n"
        "int test() { return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    /* Owning-alias globals self-initialize and are torn down. */
    STRATA_CHECK(strstr(res.output, "__strata_module_init") != NULL);
    STRATA_CHECK(strstr(res.output, "__strata_module_teardown") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(string_global_owns_and_tears_down)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "string g = \"just me\";\n"
        "int test() { return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    /* String globals use the same owning representation as every other
       owning global: a zeroed fat {ptr, len} slot, runtime init fills it,
       teardown drops it. No static constant-pool pointer, no name-keyed
       skips. */
    STRATA_CHECK(strstr(res.output, "@g = global { ptr, i64 } zeroinitializer") != NULL);
    STRATA_CHECK(strstr(res.output, "store { ptr, i64 } %sfat.l, ptr @g") != NULL);
    STRATA_CHECK(strstr(res.output, "__strata_module_teardown") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_arithmetic_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "void test() { Name a = (Name)\"x\"; Name b = (Name)\"y\"; int bad = a + b; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_arithmetic_with_string_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "void test() { string a = \"x\"; string b = \"y\"; int bad = a + b; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_move_after_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "void test() { Name n = (Name)\"a\"; Name m = n; Name dead = n; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_pass_moves_source)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "int take(Name n) { return 1; }\n"
        "void test() { Name n = (Name)\"a\"; int r = take(n); Name again = n; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_extern_return)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "extern Name host_make();\n"
        "Name test() { return host_make(); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    /* The alias crosses the extern boundary as one pointer, like string. */
    STRATA_CHECK(strstr(res.output, "declare ptr @host_make") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_extern_param_by_value)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "extern int host_len(Name n);\n"
        "int test() { Name n = (Name)\"ada\"; return host_len(n); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "declare i32 @host_len(ptr)") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_literal_cast_heap_copies)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Name = string;\n"
        "Name test() { return (Name)\"ada\"; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    /* The result owns its bytes: the static literal is duplicated. */
    STRATA_CHECK(strstr(res.output, "strata_alloc") != NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(type_alias_string_jit_cast_and_move)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct Name = string;\n"
        "int take_name(Name n) { return 1; }\n"
        "int caller() {\n"
        "    Name a = (Name)\"ada\";\n"
        "    int r = take_name(a);\n"
        "    Name b = (Name)\"bob\";\n"
        "    string raw = (string)b;\n"
        "    Name c2 = (Name)\"copy\";\n"
        "    return r;\n"
        "}\n",
        "type_alias_string", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*caller)(void) = (int (*)(void))strataJitGetFunction(jit, "caller");
        STRATA_CHECK(caller != NULL);
        if (caller)
        {
            STRATA_CHECK_EQ(caller(), 1);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        STRATA_CHECK(false);
    }
    strataCompilerDestroy(c);
}
