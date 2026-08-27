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

/* Host extern bound into JIT tests that need to observe Strata values.
   `string` crosses the boundary as `const char*`. */
static int HostStrLen(const char* s)
{
    return s ? (int)strlen(s) : -1;
}

/* Extern `T?` ABI probe: the host receives the pointer itself, NULL = empty. */
static int HostOptNonNull(const void* p)
{
    return p != NULL;
}

static const void* g_lastOpt = NULL;

static void HostOptTake(const void* p)
{
    g_lastOpt = p;
}

static StrataJit* CompileOptBound(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "opt", err);

    if (jit)
    {
        strataJitAddSymbol(jit, "str_len", (void*)&HostStrLen);
        strataJitAddSymbol(jit, "opt_non_null", (void*)&HostOptNonNull);
        strataJitAddSymbol(jit, "opt_take", (void*)&HostOptTake);
    }

    strataCompilerDestroy(c);
    return jit;
}

STRATA_TEST(optional_extern_param_and_return_abi)
{
    /* An extern `T?` param crosses as the pointer itself (NULL = empty),
       and an extern `T?` return comes back as a `Weapon?` slot that obeys
       narrowing. */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int opt_non_null(Weapon? w);\n"
        "extern void opt_take(Weapon? w);\n"
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon? empty;\n"
        "  int r = opt_non_null(empty) * 1;   // empty -> host sees NULL -> 0\n"
        "  Weapon? set = Weapon { .dmg = 7 };\n"
        "  if (set?) { r = r + opt_non_null(set); }   // set   -> 1\n"
        "  opt_take(empty);\n"
        "  return r * 10 + opt_non_null(Weapon { .dmg = 9 }); // boxed temp -> 1\n"
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
        STRATA_CHECK_EQ(entry(), 11);

        /* The last extern call passed the EMPTY optional: host saw NULL. */
        STRATA_CHECK(g_lastOpt == NULL);
    }

    strataJitDestroy(jit);
}

/* ---- The canonical example ----------------------------------------------
   struct Weapon { string name; };
   Weapon w;          -> illegal: owning struct must live in a box
   ^Weapon w = {};    -> illegal: 'name' must be initialized
   ^Weapon w = { "" } -> legal:   name is an owned empty string            */

STRATA_TEST(optional_plain_owning_struct_local_is_illegal)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Weapon { string name; };\n"
        "int main() { Weapon w; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "owning struct 'Weapon' must be stored in a box"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_empty_literal_with_string_field_is_illegal)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Weapon { string name; };\n"
        "int main() { ^Weapon w = {}; return 0; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "owning field 'name' of struct 'Weapon' must be initialized"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_empty_string_field_init_runs_and_owns)
{
    /* `^Weapon w = { "" };` compiles, the entry runs, and `name` is a real,
       owned empty string (length 0 through a host extern). */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "struct Weapon { string name; };\n"
        "int main() {\n"
        "  //Weapon w;\n"
        "  ^Weapon w = { \"\" };\n"
        "  int n = str_len(w.name);\n"
        "  w.name = \"x\";\n"          // field is mutable and owning
        "  return n * 10 + str_len(w.name);\n"
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    int (*mainFn)(void) = (int (*)(void))strataJitGetFunction(jit, "main");
    STRATA_CHECK(mainFn != NULL);
    if (mainFn)
    {
        STRATA_CHECK_EQ(mainFn(), 1); // 0 * 10 + 1
    }

    strataJitDestroy(jit);
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
        "    w.model = Model { .name = \"m\", .v = 42 };\n"
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

/* ---- Definitely-empty facts (else branch) ------------------------------- */

STRATA_TEST(optional_else_branch_reports_definitely_empty)
{
    /* Inside the else of `if (w?)`, reading through `w` gets the sharper
       "is definitely empty" diagnostic rather than "may be empty". */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Model { int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w;\n"
        "  if (w?) { }\n"
        "  else { return w.model.v; }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'w' is definitely empty"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_initialized_declaration_establishes_fact)
{
    /* `Weapon? w = Weapon {};` proves `w` non-empty for everything after -
       testing a field through it needs no outer `if (w?)`. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w.model?) { return w.model.v; }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_assignment_inside_else_clears_empty_fact)
{
    /* Assigning in the else block re-proves the path: reads after the
       assignment are legal even though we entered via "definitely empty". */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w.model?) { }\n"
        "  else\n"
        "  {\n"
        "    w.model = Model { .v = 5 };\n"
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

STRATA_TEST(optional_empty_fact_does_not_escape_else)
{
    /* Past the if/else join the path is merely unproven again - assigning
       in the OTHER branch is what makes the lazy-init join work. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    /* Without any branch proving non-empty, the post-join read errors... */
    ParseAndResolve(
        "struct Model { int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w.model?) { }\n"
        "  else { }\n"
        "  return w.model.v;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* ...but with an assignment in the else, BOTH branches prove it and
       the lazy-init idiom joins to a non-empty fact. */
    arena_init(&arena, 0);
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Model { int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  if (w.model?) { return w.model.v; }\n"
        "  else { w.model = Model { .v = 7 }; }\n"
        "  return w.model.v;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_jit_lazy_init_join_runs)
{
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "struct Model { string name; int v; };\n"
        "struct Weapon { Model? model; };\n"
        "int entry() {\n"
        "  Weapon? w = Weapon {};\n"
        "  int r = 0;\n"
        "  if (w.model?)\n"
        "  {\n"
        "    r = w.model.v;\n"
        "  }\n"
        "  else\n"
        "  {\n"
        "    w.model = Model { .name = \"fallback\", .v = 40 };\n"
        "    r = str_len(w.model.name);\n"   // 8
        "  }\n"
        "  r = r + w.model.v;               // 8 + 40 = 48\n"
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
        STRATA_CHECK_EQ(entry(), 48);
    }

    strataJitDestroy(jit);
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
        "    w.model = Model { .name = \"m\", .v = 40 };\n"
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

/* ---- Any-kind-of-deref rule ----------------------------------------------
   Reading a T?'s CONTENTS anywhere (not just member access) requires a
   narrowing fact: call args to plain T params, extern string params, bare
   extern `...` slots, returns, assignments, array elements. Box-shaped
   targets (T? / ^T params, T? decls) rebind instead and stay legal. */

static bool OptDerefRejected(const char* src, const char* needle, Arena* arena)
{
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(src, &diag, arena);
    bool hasErrors = DiagHasErrors(&diag);

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, arena);
    bool hit = Contains(d, needle);

    DiagnosticEngineFree(&diag);

    return hasErrors && hit;
}

STRATA_TEST(optional_deref_as_extern_string_arg_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int puts(string s);\n"
        "int entry() { string? s; puts(s); return 0; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_as_plain_param_arg_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Weapon { int dmg; };\n"
        "int take(Weapon w) { return w.dmg; }\n"
        "int entry() { Weapon? w; return take(w); }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'w' may be empty"));

    DiagnosticEngineFree(&diag);

    arena_free(&arena);
}

STRATA_TEST(optional_deref_in_c_vararg_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int printf(string fmt, ...);\n"
        "int entry() { string? s; printf(\"%s\", s); return 0; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_on_return_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "string f() { string? s; return s; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_on_assignment_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    /* plain string target */
    STRATA_CHECK(OptDerefRejected(
        "int entry() { string? s; string t = \"x\"; t = s; return 0; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_content_assign_into_box_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() { Weapon? w; ^Weapon b = Weapon { .dmg = 1 }; b = w; return 0; }\n",
        "'w' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_array_push_element_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() { Weapon[] arr; Weapon? v; array_push(arr, v); return 0; }\n",
        "'v' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_struct_field_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Holder { string name; };\n"
        "int entry() { string? s; Holder h = Holder { .name = s }; return 0; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_narrowed_derefs_are_legal)
{
    /* The mirror positives: same derefs inside `if (opt?)` compile clean,
       and the lazy-init idiom joins to a proven fact. */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "int entry() {\n"
        "  string? s;\n"
        "  if (s?) { } else { s = \"hi\"; }\n"
        "  int a = str_len(s);            // lazy-init join fact\n"
        "  string? t = \"inited\";\n"
        "  int b = str_len(t);            // initialized-decl fact\n"
        "  string? u;\n"
        "  int c = 0;\n"
        "  if (u?) { c = str_len(u); }    // narrowed extern string arg\n"
        "  return a * 100 + b;\n"
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
        STRATA_CHECK_EQ(entry(), 206);
    }

    strataJitDestroy(jit);
}

/* ---- Negated null tests (`if (!path?)`) ----------------------------------
   The fact machinery mirrors through `!`: then-branch proves EMPTY,
   else-branch (or the implicit fall-through) proves non-empty. */

STRATA_TEST(optional_negated_test_else_proves_non_empty)
{
    /* The exact idiom: else of `if (!s?)` may read through the optional. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "extern int puts(string s);\n"
        "int entry() {\n"
        "  string? s;\n"
        "  if (!s?) { puts(\"empty\"); }\n"
        "  else { puts(s); }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_negated_test_then_is_definitely_empty)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int puts(string s);\n"
        "int entry() { string? s; if (!s?) { puts(s); } return 0; }\n",
        "'s' is definitely empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_negated_implicit_else_lazy_init)
{
    /* `if (!s?) { s = v; }` proves `s` at the join: the explicit then
        assigns, the implicit else had it non-empty all along. */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "int entry() {\n"
        "  string? s;\n"
        "  if (!s?) { s = \"lazy\"; }\n"
        "  return str_len(s);\n"
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
        STRATA_CHECK_EQ(entry(), 4);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(optional_negated_while_body_sees_empty)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int puts(string s);\n"
        "int entry() { string? s = \"x\"; while (!s?) { puts(s); s = \"y\"; } return 0; }\n",
        "'s' is definitely empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_double_negation_matches_positive)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "extern int puts(string s);\n"
        "int entry() {\n"
        "  string? s = \"v\";\n"
        "  if (!!s?) { puts(s); }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- copy() with optionals ------------------------------------------------
   copy(T?) must yield an independent deep copy when non-empty and stay
   EMPTY when the source is empty (null copied as null, never dereferenced);
   the same holds for optional FIELDS inside copied structs, for T[]?
   arrays (canonical {null, 0} when empty), and for arrays of optionals. */

STRATA_TEST(copy_of_empty_optional_is_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon? e;\n"
        "  Weapon? c = copy(e);\n"                 /* empty -> empty, no deref */
        "  if (c?) { return 1; }\n"
        "  return 0;\n"
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
        STRATA_CHECK_EQ(entry(), 0);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(copy_of_nonempty_optional_deep_copies)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; string? title; };\n"
        "int entry() {\n"
        "  Weapon? orig = Weapon { .dmg = 10, .title = \"iron\" };\n"
        "  Weapon? c = copy(orig);\n"
        "  if (!c?) { return 1; }\n"
        "  if (c?)\n"
        "  {\n"
        "    int before = c.dmg;                 /* 10 */\n"
        "    if (orig?)\n"
        "    {\n"
        "      orig.dmg = 99;                    /* mutate the ORIGINAL */\n"
        "      if (orig.title?) { orig.title = \"junk\"; }\n"
        "    }\n"
        "    int afterTitle = 0;\n"
        "    if (c.title?) { afterTitle = 1; }   /* copy kept its title */\n"
        "    return before * 10 + c.dmg + afterTitle * 100;\n"   /* 100 + 10 + 100 */ 
        "  }\n"
        "  return 1;\n"
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
        STRATA_CHECK_EQ(entry(), 210);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(copy_of_box_with_optional_fields)
{
    /* Copying a ^T whose T has optional fields: empty fields copy as
       empty, non-empty fields deep-copy independently. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "struct Holder { string name; Weapon? w; Weapon? spare; };\n"
        "int entry() {\n"
        "  ^Holder h = Holder { .name = \"h\", .w = Weapon { .dmg = 7 } };\n"   /* spare omitted */
        "  ^Holder hc = copy(h);\n"
        "  int r = 0;\n"
        "  if (hc.w?) { r += hc.w.dmg; }\n"        /* 7 - deep-copied */ 
        "  if (hc.spare?) { r += 1000; }\n"        /* empty stays empty */ 
        "  if (h.w?) { h.w.dmg = 50; }\n"          /* mutate the original */ 
        "  if (hc.w?) { r += hc.w.dmg * 10; }\n"   /* 70 - copy unaffected */ 
        "  return r;\n"                            /* 77 */ 
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
        STRATA_CHECK_EQ(entry(), 77);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(copy_of_optional_array_empty_and_full)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "int entry() {\n"
        "  int[]? e;\n"
        "  int[]? ec = copy(e);\n"
        "  int r = 0;\n"
        "  if (ec?) { r += 1000; }\n"              /* empty -> canonical empty */ 
        "  int[]? a = {1, 2, 3};\n"
        "  int[]? ac = copy(a);\n"
        "  if (ac?)\n"
        "  {\n"
        "    a[0] = 99;                            /* mutate the original */ \n"
        "    r += ac[0] + ac[1] + ac[2];           /* 6 */\n"
        "    r += (int)ac.length;                  /* 3 */\n"
        "  }\n"
        "  return r;\n"                            /* 9 */
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
        STRATA_CHECK_EQ(entry(), 9);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(copy_of_array_of_optionals)
{
    /* Per-element deep copy: mixed non-empty/empty elements must survive
       the copy, each element independent. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] arr;\n"
        "  array_push(arr, Weapon { .dmg = 5 });\n"
        "  Weapon? e;\n"
        "  array_push(arr, e);\n"                  /* empty element */ 
        "  array_push(arr, Weapon { .dmg = 8 });\n"
        "  Weapon?[] cp = copy(arr);\n"
        "  int r = (int)cp.length * 100;\n"        /* 300 */
        "  if (cp[0]?) { r += cp[0].dmg; }\n"      /* +5 */
        "  if (cp[1]?) { r += 1000; } else { r += 1; }\n"   /* empty */
        "  if (cp[2]?)\n"
        "  {\n"
        "    r += cp[2].dmg;\n"                    /* +8 */
        "    if (arr[2]?) { arr[2] = Weapon { .dmg = 70 }; }\n"   /* rebind orig */
        "    if (cp[2]?) { r += cp[2].dmg; }\n"    /* +8, copy unaffected */
        "  }\n"
        "  return r;\n"                            /* 322 */
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
        STRATA_CHECK_EQ(entry(), 322);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(copy_of_non_owning_value_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry() { int x = 42; int y = copy(x); return y; }\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'copy' expects an owning type"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Index-precise narrowing facts ----------------------------------------
   Element keys are spelled precisely: "arr[0]" (constant) or "arr[i]"
   (dependency-tracked local). Proving one element proves ONLY that element;
   mutating the index variable, passing it as a non-const ref, resizing or
   popping the array, or calling anything that could touch globals drops
   the affected facts. */

STRATA_TEST(narrowed_element_fact_is_index_precise)
{
    /* The motivating bug: proving arr[0] must NOT prove arr[9]. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int puts(string s);\n"
        "struct Node { string str; Node? next; };\n"
        "int entry() {\n"
        "  ^Node[] a = { Node(\"x\") };\n"
        "  if (a[0].next?)\n"
        "  {\n"
        "    puts(a[9].next.str);\n"      /* different element - unproven */
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'a[9].next' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_same_index_runs)
{
    /* The mirror positive: the SAME (constant) index stays proven. */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "struct Node { string str; Node? next; };\n"
        "int entry() {\n"
        "  ^Node[] a = { Node(\"me\", Node(\"child\")) };\n"
        "  int r = 0;\n"
        "  if (a[0].next?)\n"
        "  {\n"
        "    r += str_len(a[0].next.str);\n"     /* 5 - same element */ 
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
        STRATA_CHECK_EQ(entry(), 5);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(narrowed_element_variable_index_runs)
{
    /* A tracked local index is precise: test and use agree on `i`. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack;\n"
        "  array_push(rack, Weapon { .dmg = 3 });\n"
        "  Weapon? e;\n"
        "  array_push(rack, e);\n"
        "  array_push(rack, Weapon { .dmg = 7 });\n"
        "  int sum = 0;\n"
        "  for (uint i = 0; i < rack.length; i++)\n"
        "  {\n"
        "    if (rack[i]?) { sum += rack[i].dmg; }\n"    /* 3 + 7 */
        "  }\n"
        "  return sum;\n"
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
        STRATA_CHECK_EQ(entry(), 10);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(narrowed_element_index_mutation_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[i]?)\n"
        "  {\n"
        "    i = 1;\n"                     /* the index changed under us */
        "    return rack[i].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_index_ref_pass_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "void bump(ref uint n) { n = n + 1; }\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[i]?)\n"
        "  {\n"
        "    bump(i);\n"                   /* callee may mutate the index */
        "    return rack[i].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_complex_index_is_refused)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[i + 1]?) { }\n"
        "  return 0;\n"
        "}\n"
        "int probe() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[i + 1]?)\n"
        "  {\n"
        "    return rack[i + 1].dmg;\n"    /* complex index: never provable */
        "  }\n"
        "  return 0;\n"
        "}\n",
        "not a constant or a trackable variable", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_resize_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  if (rack[0]?)\n"
        "  {\n"
        "    array_resize(rack, 0);\n"     /* dropped the element we proved */
        "    return rack[0].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_global_fact_dies_across_call)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "Weapon? gw = Weapon { .dmg = 3 };\n"
        "int side() { return 1; }\n"
        "int entry() {\n"
        "  if (gw?)\n"
        "  {\n"
        "    side();\n"                    /* any call may rebind the global */
        "    return gw.dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_survives_array_push)
{
    /* push preserves existing element values - the fact stays valid. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  int r = 0;\n"
        "  if (rack[0]?)\n"
        "  {\n"
        "    array_push(rack, Weapon { .dmg = 9 });\n"
        "    r += rack[0].dmg;\n"          /* 3 - fact survived the push */
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
        STRATA_CHECK_EQ(entry(), 3);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(literal_element_move_does_not_poison_siblings)
{
    /* Precise keys make literal-index moves element-exact: moving rack[0]
       leaves rack[1] usable (the erased form used to poison both). */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Weapon { int dmg; };\n"
        "int take(^Weapon w) { return w.dmg; }\n"
        "int entry() {\n"
        "  ^Weapon[] rack = { Weapon { .dmg = 3 }, Weapon { .dmg = 4 } };\n"
        "  int a = take(rack[0]);\n"       /* moves element 0 */
        "  int b = take(rack[1]);\n"       /* sibling unaffected */
        "  return a + b;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- `while (foo?)` condition narrowing -----------------------------------
   The loop condition is re-tested every iteration, so its fact holds
   throughout the body - including for call args that unwrap the optional
   (checked BEFORE move-tracking clears the fact by moving the value out).
   No fact survives the loop: the condition was false on the last check. */

STRATA_TEST(optional_while_condition_narrows_call_args)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "extern int puts(string s);\n"
        "string next(string v) { return v; }\n"
        "int entry() {\n"
        "  string? s = \"x\";\n"
        "  while (s?)\n"
        "  {\n"
        "    puts(s);          // extern string param, narrowed by the condition\n"
        "    s = next(s);      // owning param: deref checked, then the move tracked\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(optional_while_fact_does_not_survive_loop)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int puts(string s);\n"
        "int entry() { string? s = \"x\"; while (s?) { puts(s); } puts(s); return 0; }\n",
        "'s' may be empty", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_jit_while_walk_passes_narrowed_arg)
{
    /* The classic chain walk, with the narrowed optional itself passed to
       a plain-T param each iteration (`visit(cur)` unwraps `cur`). */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Cell { int n; Cell? next; };\n"
        "int visit(Cell c) { return c.n; }\n"
        "int entry() {\n"
        "  ^Cell c = Cell { .n = 3 };\n"
        "  ^Cell b = Cell { .n = 2, .next = c };\n"
        "  ^Cell a = Cell { .n = 1, .next = b };\n"
        "  Cell? cur = a;\n"
        "  int total = 0;\n"
        "  while (cur?)\n"
        "  {\n"
        "    total = total * 10 + visit(cur);   // deref of cur as a call arg\n"
        "    cur = cur.next;\n"
        "  }\n"
        "  return total;\n"
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
        STRATA_CHECK_EQ(entry(), 123);
    }

    strataJitDestroy(jit);
}
