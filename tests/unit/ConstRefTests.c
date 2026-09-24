#include "Util.h"
#include "Sema/ResolveOverloads.h"
#include "Test.h"

#if STRATA_TEST_HAS_LLVM
#include "Codegen/CodegenBackend.h"
#include "strata/strata.h"
#endif

#include <string.h>

/* ---- Parse + sema ---- */

STRATA_TEST(const_ref_scalar_parses_and_resolves)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int read(const ref int x) { return x; }\n"
        "int entry() { int v = 7; return read(v); }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count >= 1);

    FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(f->params.count == 1);

    ParamDecl* p = (ParamDecl*)VecGet(&f->params, 0);
    STRATA_CHECK(p->mod == ModRef);       /* by-reference */
    STRATA_CHECK(p->type.isConst);        /* read-only */

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(const_ref_and_ref_const_both_accepted)
{
    /* Either token order should parse to the same (by-ref, immutable) param. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int a(const ref int x) { return x; }\n"
        "int b(ref const int x) { return x; }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fa = (FunctionDecl*)VecGet(&mod->functions, 0);
    FunctionDecl* fb = (FunctionDecl*)VecGet(&mod->functions, 1);
    ParamDecl* pa = (ParamDecl*)VecGet(&fa->params, 0);
    ParamDecl* pb = (ParamDecl*)VecGet(&fb->params, 0);

    STRATA_CHECK(pa->mod == ModRef && pa->type.isConst);
    STRATA_CHECK(pb->mod == ModRef && pb->type.isConst);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(const_ref_struct_is_allowed)
{
    /* Structs pass by value by default, so `const ref` is the explicit
       read-only VIEW spelling - it must not be rejected. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct V { float x; float y; };\n"
        "float first(const ref V v) { return v.x; }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(assign_to_const_ref_scalar_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "int bad(const ref int x) { x = 5; return x; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(compound_assign_to_const_ref_scalar_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "int bad(const ref int x) { x += 5; return x; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(assign_to_const_ref_struct_member_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "struct V { float x; float y; };\n"
        "int bad(const ref V v) { v.y = 9.0; return 0; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Const locals and const bypasses ---- */

/* True when `src` fails sema with a diagnostic containing `needle`. */
static bool ConstRejected(const char* src, const char* needle)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(src, &diag, &arena);

    SourceManager sm; SourceManagerInit(&sm);
    bool hit = DiagHasErrors(&diag) && strstr(DiagFormat(&diag, &sm, 1, &arena), needle) != NULL;

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return hit;
}

static bool ConstAccepted(const char* src)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(src, &diag, &arena);

    bool ok = !DiagHasErrors(&diag);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return ok;
}

STRATA_TEST(const_local_assign_and_incdec_are_errors)
{
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x = 6; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x++; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x += 1; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int[] a = {1, 2}; a[0] = 3; return a[0]; }\n",
                               "'a' is immutable"));
}

STRATA_TEST(const_local_is_readable_and_scoped)
{
    /* A const local can be read and copied; a later sibling-scope name is a fresh mutable binding. */
    STRATA_CHECK(ConstAccepted(
        "int entry() {\n"
        "  const int x = 5; int y = x; y++;\n"
        "  if (y > 0) { const int z = 1; y = y + z; }\n"
        "  int z = 2; z = 3;\n"
        "  return y + z;\n"
        "}\n"));
}

STRATA_TEST(const_ref_arg_to_mutable_ref_param_is_error)
{
    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void m(ref P p) { p.v = 1; }\n"
        "void g(const ref P p) { m(p); }\n",
        "'p' is immutable and cannot be passed to non-const 'ref' parameter"));

    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void setv(ref int x) { x = 1; }\n"
        "void g(const ref P p) { setv(p.v); }\n",
        "'p' is immutable"));

    STRATA_CHECK(ConstRejected(
        "void setv(ref int x) { x = 1; }\n"
        "int entry() { const int x = 5; setv(x); return x; }\n",
        "'x' is immutable"));
}

STRATA_TEST(const_ref_arg_to_const_ref_param_is_allowed)
{
    STRATA_CHECK(ConstAccepted(
        "struct P { int v; };\n"
        "int m(const ref P p) { return p.v; }\n"
        "int g(const ref P p) { return m(p); }\n"
        "int h(const int[] a) { return a.length; }\n"));
}

STRATA_TEST(const_array_push_pop_resize_are_errors)
{
    STRATA_CHECK(ConstRejected("void g(const int[] a) { array_push(a, 1); }\n", "it is immutable"));
    STRATA_CHECK(ConstRejected("int g(const int[] a) { return array_pop(a); }\n", "it is immutable"));
    STRATA_CHECK(ConstRejected("void g(const int[] a) { array_resize(a, 4); }\n", "it is immutable"));
}

STRATA_TEST(const_box_drop_is_error)
{
    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void g(const ^P p) { drop(p); }\n",
        "cannot drop 'p'; it is immutable"));
}

#if STRATA_TEST_HAS_LLVM


/* ---- JIT runtime behavior ---- */

STRATA_TEST(const_ref_scalar_is_readable_view)
{
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "int read(const ref int x) { return x + 1; }\n"
        "int entry() {\n"
        "  int v = 10;\n"
        "  int r = read(v);\n"            /* 11; v unchanged (view, not move) */
        "  return r + v;\n"               /* 11 + 10 = 21 */
        "}\n",
        "cref", &err);
    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        return;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 21);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(const_ref_struct_dot_product_runs)
{
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(const ref Vec3 a, const ref Vec3 b) {\n"
        "  return a.x * b.x + a.y * b.y + a.z * b.z;\n"
        "}\n"
        "int entry() {\n"
        "  Vec3 v = Vec3 { .x = 1.0, .y = 2.0, .z = 3.0 };\n"
        "  return (int)dot(v, v);\n"      /* 1 + 4 + 9 = 14 */
        "}\n",
        "cref", &err);
    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        return;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 14);
    }

    strataJitDestroy(jit);
}

#endif
