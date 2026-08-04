#include "Codegen/CBackend.h"
#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <string.h>

STRATA_TEST(c_backend_emits_scalar_control_flow)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(
        compiler, "int sum(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i; } return s; }", "c_scalar",
        STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    STRATA_CHECK(strstr(result.output, "int sum(int strata__var_n)") != NULL);
    STRATA_CHECK(strstr(result.output, "for (") != NULL);
    STRATA_CHECK(strstr(result.output, "return strata__var_s") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(c_backend_emits_struct_pointer_abi)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct V { float x; float y; };\n"
                                  "float dot(const V a, const V b) { return a.x * b.x + a.y * b.y; }\n",
                                  &diag, &arena);
    BuiltCModule result = BuildCModule(mod, &diag, &arena, true, STRATA_ARCH_AUTO);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(result.source, "const strata__type_V* strata__var_a") != NULL);
    STRATA_CHECK(strstr(result.source, "((*strata__var_a)).strata__field_x") != NULL);
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
    Module* mod = ParseAndResolve("extern int host_add(int a, int b);\n"
                                  "int entry() { return host_add(20, 22); }\n",
                                  &diag, &arena);
    BuiltCModule result = BuildCModule(mod, &diag, &arena, true, STRATA_ARCH_AUTO);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(result.source, "int (*strata__ext_host_add)(int, int) = 0;") != NULL);
    STRATA_CHECK(strstr(result.source, "strata__ext_host_add(20, 22)") != NULL);
    STRATA_CHECK_EQ((long)result.externs.count, 1);
    BuiltCModuleDispose(&result);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(c_backend_emits_forward_struct_when_incomplete)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Foo;\n"
                                  "void use(const Foo f) {}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltCModule result = BuildCModule(mod, &diag, &arena, true, STRATA_ARCH_AUTO);
    /* Incomplete struct: forward-declared C struct typedef, no body, and NOT a
       handle pointer typedef. */
    STRATA_CHECK(strstr(result.source, "typedef struct strata__type_Foo strata__type_Foo;") != NULL);
    STRATA_CHECK(strstr(result.source, "struct strata__type_Foo {") == NULL);
    STRATA_CHECK(strstr(result.source, "strata__handle_tag_Foo") == NULL);
    BuiltCModuleDispose(&result);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(c_backend_completes_forward_declared_struct)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Foo;\n"
                                  "struct Foo { int x; };\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltCModule result = BuildCModule(mod, &diag, &arena, true, STRATA_ARCH_AUTO);
    /* A later body completes the type exactly once. */
    STRATA_CHECK(strstr(result.source, "struct strata__type_Foo {\n") != NULL);
    BuiltCModuleDispose(&result);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(c_backend_encodes_overload_symbols)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(compiler,
                                              "int add(int a, int b) { return a + b; }\n"
                                              "float add(float a, float b) { return a + b; }\n",
                                              "c_overloads", STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    STRATA_CHECK(strstr(result.output, "strata__fn_add_x24_int_x24_int") != NULL);
    STRATA_CHECK(strstr(result.output, "strata__fn_add_x24_float_x24_float") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(c_backend_orders_struct_value_dependencies)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(compiler,
                                              "struct Outer { Inner value; }; struct Inner { int x; }; "
                                              "int entry() { Outer o; o.value.x = 42; return o.value.x; }",
                                              "struct_order", STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    const char* inner = strstr(result.output, "struct strata__type_Inner {");
    const char* outer = strstr(result.output, "struct strata__type_Outer {");
    STRATA_CHECK(inner != NULL);
    STRATA_CHECK(outer != NULL);
    STRATA_CHECK(inner < outer);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(c_backend_rejects_struct_value_cycles)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result
        = strataCompileString(compiler, "struct A { B b; }; struct B { A a; };", "struct_cycle", STRATA_EMIT_C);
    STRATA_CHECK(!result.ok);
    STRATA_CHECK(strstr(result.diagnostics, "by-value dependency cycle") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}

STRATA_TEST(c_backend_escapes_c_keywords_and_emits_source_lines)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(compiler, "int auto() { return 7; } int entry() { return auto(); }",
                                              "keyword_test.strata", STRATA_EMIT_C);
    STRATA_CHECK(result.ok);
    STRATA_CHECK(strstr(result.output, "strata__fn_auto") != NULL);
    STRATA_CHECK(strstr(result.output, "#line 1 \"keyword_test.strata\"") != NULL);
    strataResultFree(&result);
    strataCompilerDestroy(compiler);
}
