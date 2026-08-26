#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

static StrataJit* CompileOpt(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "opt", err);
    strataCompilerDestroy(c);
    return jit;
}

/* ---- Sema: forced initialization of ^T fields -------------------------- */

STRATA_TEST(optional_missing_box_field_init_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Weapon { string? name; int dmg; };\n"
        "struct Inv { ^Weapon w; };\n"
        "int entry() { ^Inv i = {}; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "owning field 'w' of struct 'Inv' must be initialized"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_field_may_be_omitted_in_init)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() { ^Weapon w = {}; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_local_need_not_be_initialized)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() { Weapon? w; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Sema: null test operator ------------------------------------------ */

STRATA_TEST(optional_null_test_on_non_optional_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry() { int x = 5; if (x?) { } return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'?' test requires a nullable type"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_read_without_narrowing_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() { Weapon? w; return w.model.v; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "may be empty"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_nested_test_requires_parent_fact)
{
    /* Testing `w.model?` on an unproven optional `w` is rejected: the test
       itself would dereference an empty box to reach the field. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() { Weapon? w; if (w.model?) { } return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'w' may be empty"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_write_through_unproven_path_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() { Weapon? w; w.model = Model { .name = \"x\" }; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "may be empty"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_narrowed_inside_if_and_dead_after)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    /* Inside the branch the read is fine... */
    Module* ok = ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w?) { if (w.model?) { return w.model.v; } }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(ok != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* ...but the fact does not survive past a conservative join. */
    arena_init(&arena, 0);
    DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Model { string name; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w?) { }\n"
        "  return w.model.v;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_definite_reassignment_establishes_fact)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { string name; int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w?)\n"
        "  {\n"
        "    w.model = Model { .v = 42 };\n"
        "    return w.model.v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Runtime (JIT): optionals behave at execution ----------------------- */

STRATA_TEST(optional_jit_empty_test_and_narrowed_walk)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Model { string name; int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w;\n"
        "  int r = 0;\n"
        "  if (w?) { r = 1; }\n"
        "  else { r = 2; }\n"
        "  w = Weapon {};\n"
        "  if (w?)\n"
        "  {\n"
        "    w.model = Model { .v = 40 };\n"
        "    r = r + w.model.v;\n"
        "  }\n"
        "  return r;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 42);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(optional_jit_string_array_and_list_moves)
{
    /* Exercises optional strings (field + read), arrays of optionals
       (push moves a ^T and boxes a literal), and the linked-list walk
       (`cur = cur.next` aliased move) end to end. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Model { int polys; };\n"
        "struct Weapon { Model? model; string? title; int dmg; };\n"
        "struct Item { int value; Item? next; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = {};\n"
        "  ^Weapon sword = Weapon { .dmg = 30 };\n"
        "  sword.title = \"sword\";\n"
        "  array_push(rack, sword);\n"
        "  array_push(rack, Weapon { .dmg = 55 });\n"
        "  int rackSum = 0;\n"
        "  for (int i = 0; i < rack.length; i++)\n"
        "  {\n"
        "    if (rack[i]?) { rackSum += rack[i].dmg; }\n"
        "  }\n"
        "  ^Item c = Item { .value = 3 };\n"
        "  ^Item b = Item { .value = 2, .next = c };\n"
        "  ^Item a = Item { .value = 1, .next = b };\n"
        "  Item? cur = a;\n"
        "  int listSum = 0;\n"
        "  while (cur?)\n"
        "  {\n"
        "    listSum += cur.value;\n"
        "    cur = cur.next;\n"
        "  }\n"
        "  return rackSum * 100 + listSum;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 8506);
    }

    strataJitDestroy(jit);
}
