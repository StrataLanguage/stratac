#include "Util.h"
#if STRATA_TEST_HAS_LLVM
#include "Codegen/CodegenBackend.h"
#endif
#include "Sema/ResolveOverloads.h"
#include "Test.h"

#include <string.h>

#include "strata/strata.h"

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

STRATA_TEST(single_function_keeps_base_name)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("int only(int a) { return a; }\nint entry() { return only(5); }\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* f0 = (FunctionDecl*)VecGet(&mod->functions, 0);
    FunctionDecl* f1 = (FunctionDecl*)VecGet(&mod->functions, 1);
    STRATA_CHECK(strcmp(f0->mangledName, "only") == 0);
    STRATA_CHECK(strcmp(f1->mangledName, "entry") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(overloads_get_mangled_names_and_resolve)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n"
        "int entry_i() { return add(2, 3); }\n"
        "float entry_f() { return add(2.0, 3.0); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* f0 = (FunctionDecl*)VecGet(&mod->functions, 0);
    FunctionDecl* f1 = (FunctionDecl*)VecGet(&mod->functions, 1);
    STRATA_CHECK(strcmp(f0->mangledName, "add$int$int") == 0);
    STRATA_CHECK(strcmp(f1->mangledName, "add$float$float") == 0);

    FunctionDecl* f2 = (FunctionDecl*)VecGet(&mod->functions, 2);
    Block* callsI = (Block*)f2->body;
    ReturnStmt* retI = (ReturnStmt*)VecGet(&callsI->statements, 0);
    STRATA_CHECK(strcmp(((CallExpr*)retI->value)->callee, "add$int$int") == 0);

    FunctionDecl* f3 = (FunctionDecl*)VecGet(&mod->functions, 3);
    Block* callsF = (Block*)f3->body;
    ReturnStmt* retF = (ReturnStmt*)VecGet(&callsF->statements, 0);
    STRATA_CHECK(strcmp(((CallExpr*)retF->value)->callee, "add$float$float") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(llvm_emits_distinct_overload_symbols)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n"
        "int entry_i() { return add(2, 3); }\n"
        "float entry_f() { return add(2.0, 3.0); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @\"add$int$int\""));
    STRATA_CHECK(Contains(res.output, "define float @\"add$float$float\""));
    STRATA_CHECK(Contains(res.output, "call i32 @\"add$int$int\""));
    STRATA_CHECK(Contains(res.output, "call float @\"add$float$float\""));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
#endif


STRATA_TEST(struct_vs_scalar_overload_resolves)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Vec3 { float x; float y; float z; };\n"
        "float mag(const Vec3 v) { return v.x + v.y + v.z; }\n"
        "float mag(float s) { return s; }\n"
        "float ev() { Vec3 v; v.x = 1.0; v.y = 2.0; v.z = 3.0; return mag(v); }\n"
        "float es() { return mag(10.0); }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* f0 = (FunctionDecl*)VecGet(&mod->functions, 0);
    FunctionDecl* f1 = (FunctionDecl*)VecGet(&mod->functions, 1);
    STRATA_CHECK(strcmp(f0->mangledName, "mag$Vec3") == 0);
    STRATA_CHECK(strcmp(f1->mangledName, "mag$float") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(ambiguous_overload_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int f(int a, float b) { return 1; }\n"
        "int f(float a, int b) { return 2; }\n"
        "int entry() { return f(1, 2); }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(no_matching_overload_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { float x; };\n"
        "int f(int a) { return a; }\n"
        "int entry() { V v; return f(v); }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(undefined_function_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int entry() { return foofdofdofd(); }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "unknown function 'foofdofdofd'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(returning_void_value_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "void shoot(int x) { }\n"
        "int entry() { return shoot(1); }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot return a value of type 'void'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(returning_mismatched_type_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int f() { return \"hello\"; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot return a value of type 'string' from a function returning 'int'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(returning_array_of_wrong_element_type_is_an_error)
{
    /* Returning ^string[] from a string[] function must be a compile-time
       type error, not a miscompile that crashes at runtime (the elements are
       char**, not char*). */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "^string[] g = { \"Hi\" };\n"
        "string[] f() { return g; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot return a value of type '^string[]'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(constructor_call_is_not_unknown)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Vec3 { float x; float y; float z; };\n"
        "int entry() { Vec3 v = Vec3(1, 2, 3); return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(bare_struct_param_is_allowed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { float x; };\nextern void take(V v);\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(const_struct_param_is_read_only)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { float x; };\nvoid take(const V v) { v.x = 1.0; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(bare_struct_param_is_mutable)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { float x; };\nvoid take(V v) { v.x = 1.0; }\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(struct_inout_param_is_allowed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { float x; };\nvoid bump(V v) { v.x = v.x + 1.0; }\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(extern_struct_param_with_in_is_ok)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { float x; };\n"
        "extern void take(const V v);\n"
        "extern void fill(V v);\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(extern_cannot_return_struct_by_value)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { float x; };\nextern V make();\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_local_is_incomplete_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo;\nint entry() { Foo f; return 0; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "incomplete type 'Foo'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_field_is_incomplete_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo;\nstruct Bar { Foo f; };\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "incomplete type 'Foo'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_return_is_incomplete_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo;\nFoo make();\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "incomplete type 'Foo'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_member_access_is_incomplete_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo;\nvoid use(const Foo f) { int x; x = f.x; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "incomplete type 'Foo'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_param_is_allowed)
{
    /* An incomplete struct may appear as a (pointer-passed) parameter. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo;\nvoid use(const Foo f) {}\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(cast_scalar_to_scalar_is_allowed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry() {\n"
        "  float f = 2.0;\n"
        "  int x = (int)f;\n"
        "  long l = (long)x;\n"
        "  bool b = (bool)l;\n"
        "  return (int)b;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(cast_handle_downcast_in_lineage_is_allowed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "handle Entity;\n"
        "handle Player extends Entity;\n"
        "extern Entity spawn();\n"
        "int entry() { Entity e = spawn(); Player p = (Player)e; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(cast_between_unrelated_handles_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "handle Entity;\n"
        "handle Camera;\n"
        "extern Entity spawn();\n"
        "int entry() { Entity e = spawn(); Camera c = (Camera)e; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "invalid cast"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(cast_between_handle_and_int_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "handle Entity;\n"
        "extern Entity spawn();\n"
        "int entry() { Entity e = spawn(); int x = (int)e; return x; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "invalid cast from 'Entity' to 'int'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(cast_struct_to_scalar_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { float x; };\n"
        "int entry() { V v; int x = (int)v; return x; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "invalid cast from 'V' to 'int'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(handle_param_does_not_need_direction)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Entity;\nextern int id_of(Entity e);\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(handle_cannot_have_members_accessed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "handle Entity;\n"
        "extern Entity make();\n"
        "float entry() { Entity e = make(); return e.x; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(in_scalar_param_is_const)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("void foo(const int x) { x = 5; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(in_scalar_param_compound_assign_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("void foo(const int x) { x += 5; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(in_struct_param_member_is_const)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Vec3 { float x; float y; float z; };\nvoid foo(const Vec3 v) { v.y = 3.0; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(in_param_can_be_read)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("int foo(const int x) { return x + 1; }\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

