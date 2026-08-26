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
    /* Structs are implicitly by-ref, so `const ref` is the explicit spelling
       of the same thing - it must not be rejected. */
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

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 21);
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

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 14);
    }

    strataJitDestroy(jit);
}

#endif
