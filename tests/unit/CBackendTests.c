#include "Codegen/CBackend.h"
#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <string.h>

STRATA_TEST(c_backend_emits_scalar_control_flow)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(
        compiler,
        "int sum(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i; } return s; }",
        "c_scalar", STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    STRATA_CHECK(strstr(result.output, "int sum(int __strata_var_n)") != NULL);
    STRATA_CHECK(strstr(result.output, "for (") != NULL);
    STRATA_CHECK(strstr(result.output, "return __strata_var_s") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(c_backend_emits_struct_pointer_abi)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct V { float x; float y; };\n"
        "float dot(const V a, const V b) { return a.x * b.x + a.y * b.y; }\n",
        &diag, &arena);
    BuiltCModule result = BuildCModule(mod, &diag, &arena, true);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(result.source, "const __strata_type_V* __strata_var_a") != NULL);
    STRATA_CHECK(strstr(result.source, "((*__strata_var_a)).__strata_field_x") != NULL);
    BuiltCModuleDispose(&result);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(c_backend_emits_typed_extern_slots)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "extern int host_add(int a, int b);\n"
        "int entry() { return host_add(20, 22); }\n",
        &diag, &arena);
    BuiltCModule result = BuildCModule(mod, &diag, &arena, true);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(result.source, "int (*__strata_ext_host_add)(int, int) = 0;") != NULL);
    STRATA_CHECK(strstr(result.source, "__strata_ext_host_add(20, 22)") != NULL);
    STRATA_CHECK_EQ((long)result.externs.count, 1);
    BuiltCModuleDispose(&result);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(c_backend_encodes_overload_symbols)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(
        compiler,
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n",
        "c_overloads", STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    STRATA_CHECK(strstr(result.output, "__strata_fn_add_x24_int_x24_int") != NULL);
    STRATA_CHECK(strstr(result.output, "__strata_fn_add_x24_float_x24_float") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}
