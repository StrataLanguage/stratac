#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <string.h>

// Regression tests for sibling-scope leakage: a variable declared inside one
// block must not be visible in an adjacent block or after the block exits.
// Currently these FAIL (no diagnostic) because sema threads a single flat
// scope map through every block.

STRATA_TEST(scope_sibling_if_blocks_do_not_share_locals)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c != 0)\n"
        "    {\n"
        "        if (c == 1)\n"
        "        {\n"
        "            int move_direction = 10;\n"
        "            result += move_direction;\n"
        "        }\n"
        "        if (c == 2)\n"
        "        {\n"
        "            result += move_direction;\n"
        "        }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_then_local_not_visible_in_else)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c == 1)\n"
        "    {\n"
        "        int x = 10;\n"
        "        result += x;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        result += x;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_block_local_not_visible_after_block)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c == 1)\n"
        "    {\n"
        "        int y = 5;\n"
        "        result += y;\n"
        "    }\n"
        "    result += y;\n"
        "    return result;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_inner_block_can_read_outer_local)
{
    // Sanity: nesting must still work - inner blocks see outer locals.
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c != 0)\n"
        "    {\n"
        "        int speed = 2;\n"
        "        if (c == 1)\n"
        "        {\n"
        "            result += speed;\n"
        "        }\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_sibling_blocks_may_reuse_a_name)
{
    // Same name in adjacent blocks is fine: the first is gone when the
    // second is declared.
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c == 1)\n"
        "    {\n"
        "        int x = 1;\n"
        "        result += x;\n"
        "    }\n"
        "    if (c == 2)\n"
        "    {\n"
        "        int x = 2;\n"
        "        result += x;\n"
        "    }\n"
        "    return result;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_shadowing_outer_is_an_error)
{
    // No shadowing: redeclaring a visible outer name is a redefinition.
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int x = 1;\n"
        "    if (c == 1)\n"
        "    {\n"
        "        int x = 2;\n"
        "        x += 1;\n"
        "    }\n"
        "    return x;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_same_block_redeclare_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry()\n"
        "{\n"
        "    int x = 1;\n"
        "    int x = 2;\n"
        "    return x;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_for_header_var_does_not_leak)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry()\n"
        "{\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < 10; i = i + 1) { s += i; }\n"
        "    s += i;\n"
        "    return s;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_while_body_var_does_not_leak)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry(int c)\n"
        "{\n"
        "    int s = 0;\n"
        "    while (c > 0)\n"
        "    {\n"
        "        int step = 1;\n"
        "        s += step;\n"
        "        c = c - 1;\n"
        "    }\n"
        "    s += step;\n"
        "    return s;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(scope_sibling_reuse_runs_correctly)
{
    // End-to-end: sibling blocks reusing a name must read their OWN slot.
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(
        c,
        "int entry(int c)\n"
        "{\n"
        "    int result = 0;\n"
        "    if (c == 1) { int x = 10; result += x; }\n"
        "    if (c == 2) { int x = 20; result += x; }\n"
        "    return result;\n"
        "}\n",
        "scope", &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(int) = (int (*)(int))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(1), 10);
            STRATA_CHECK_EQ(entry(2), 20);
            STRATA_CHECK_EQ(entry(3), 0);
        }

        strataJitDestroy(jit);
    }
    else if (err)
    {
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);
}
