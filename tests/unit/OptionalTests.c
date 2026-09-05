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
    STRATA_CHECK(Contains(d, "has not been blessed"));

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
    STRATA_CHECK(Contains(d, "'w' has not been blessed"));

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
    STRATA_CHECK(Contains(d, "has not been blessed"));

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
       "is definitely empty" diagnostic rather than "has not been blessed". */
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
        "'s' has not been blessed", &arena));

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
    STRATA_CHECK(Contains(d, "'w' has not been blessed"));

    DiagnosticEngineFree(&diag);

    arena_free(&arena);
}

STRATA_TEST(optional_deref_in_c_vararg_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "extern int printf(string fmt, ...);\n"
        "int entry() { string? s; printf(\"%s\", s); return 0; }\n",
        "'s' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_on_return_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "string f() { string? s; return s; }\n",
        "'s' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_on_assignment_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    /* plain string target */
    STRATA_CHECK(OptDerefRejected(
        "int entry() { string? s; string t = \"x\"; t = s; return 0; }\n",
        "'s' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_content_assign_into_box_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() { Weapon? w; ^Weapon b = Weapon { .dmg = 1 }; b = w; return 0; }\n",
        "'w' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_array_push_element_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() { Weapon[] arr; Weapon? v; array_push(arr, v); return 0; }\n",
        "'v' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_deref_struct_field_requires_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Holder { string name; };\n"
        "int entry() { string? s; Holder h = Holder { .name = s }; return 0; }\n",
        "'s' has not been blessed", &arena));

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

STRATA_TEST(optional_negated_test_return_blesses_at_join)
{
    /* `if (!s?) { return 1; }` - the then arm terminates, so only the
       non-empty path reaches the join: reading through `s` is legal. */
    const char* err = NULL;
    StrataJit* jit = CompileOptBound(
        "extern int str_len(string s);\n"
        "int entry() {\n"
        "  string? s = \"abcd\";\n"
        "  if (!s?) { return 1; }\n"
        "  return str_len(s);\n"              /* 4 */
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

STRATA_TEST(optional_else_return_keeps_then_fact_at_join)
{
    /* The else arm terminates: only the (blessed) then path reaches the
       join, so the fact survives without intersecting with the dead else. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "extern int puts(string s);\n"
        "int entry() {\n"
        "  string? s = \"x\";\n"
        "  if (s?) { puts(s); }\n"
        "  else { return 1; }\n"
        "  puts(s);\n"                        /* still blessed on the only path */
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Facts across loops and `ref` borrows ---------------------------------
   A `ref T` param borrows the UNWRAPPED pointee of a `T?` argument - the
   callee cannot rebind (or empty) the caller's slot, so the fact survives.
   Only a `ref T?` param crosses the slot itself and kills the fact. A
   pre-loop fact survives the loop when the body never invalidates it (the
   zero-iteration path holds it); a positive `while (e?)` exits with `e`
   empty, so its fact never survives. */

static int g_optTouchCount = 0;

static void HostOptTouch(int* self)
{
    g_optTouchCount++;
    *self = *self + 1;
}

STRATA_TEST(optional_fact_survives_ref_method_calls_and_loops)
{
    /* The engine-main idiom: bless via early return, then read through the
       optional inside and after a loop whose body only ref-borrows it. */
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "struct E { int v; };\n"
        "impl E { extern void Touch(ref E self); }\n"
        "int entry() {\n"
        "  E? e = E { .v = 5 };\n"
        "  if (!e?) { return -1; }\n"
        "  int i = 0;\n"
        "  while (i < 3) {\n"
        "    e.Touch();\n"                    /* ref borrow: fact survives */
        "    i = i + 1;\n"
        "  }\n"
        "  e.Touch();\n"
        "  return e.v;\n"                     /* 5 + 4 touches = 9 */
        "}\n",
        "opt",
        &err);

    if (jit)
    {
        strataJitAddSymbol(jit, "E_Touch", (void*)&HostOptTouch);
    }

    strataCompilerDestroy(c);

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

STRATA_TEST(optional_ref_slot_param_still_kills_fact)
{
    /* A `ref T?` param crosses the slot itself: the callee may rebind it,
       so the blessing dies at the call. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct E { int v; };\n"
        "extern void Rebind(ref E? slot);\n"
        "int entry() {\n"
        "  E? e = E { .v = 1 };\n"
        "  Rebind(e);\n"
        "  return e.v;\n"
        "}\n",
        "'e' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_positive_while_cond_is_empty_after_loop)
{
    /* `while (e?)` exits when e is empty: reading through it afterwards
       must still be rejected. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct E { int v; };\n"
        "int entry() {\n"
        "  E? e = E { .v = 1 };\n"
        "  while (e?) { e.v = e.v + 1; }\n"
        "  return e.v;\n"
        "}\n",
        "'e'", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_loop_reassign_from_unprovable_kills_fact)
{
    /* Reassigning the slot inside the loop (maybe-empty result) invalidates
       the pre-loop blessing at the exit. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct E { int v; };\n"
        "E? Make() { E? r = E { .v = 1 }; return r; }\n"
        "int entry() {\n"
        "  E? e = Make();\n"
        "  if (!e?) { return 1; }\n"
        "  int i = 0;\n"
        "  while (i < 3) { e = Make(); i = i + 1; }\n"
        "  return e.v;\n"
        "}\n",
        "'e' has not been blessed", &arena));

    arena_free(&arena);
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
        "'a[9].next' has not been blessed", &arena));

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
        "has not been blessed", &arena));

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
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_arithmetic_index_runs)
{
    /* Complex indices are pinned by a canonical fully-parenthesized
       spelling: test and use of the SAME expression agree. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack;\n"
        "  array_push(rack, Weapon { .dmg = 3 });\n"
        "  array_push(rack, Weapon { .dmg = 7 });\n"   /* rack[1] */
        "  Weapon? e;\n"
        "  array_push(rack, e);\n"                     /* rack[2] empty */
        "  uint i = 0;\n"
        "  int r = 0;\n"
        "  if (rack[i + 1]?)\n"
        "  {\n"
        "    r += rack[i + 1].dmg;\n"              /* 7 */ 
        "  }\n"
        "  if (rack[(i + 1) * 2 / 2]?)\n"          /* same value, same spelling */ 
        "  {\n"
        "    r += rack[(i + 1) * 2 / 2].dmg;\n"    /* +7 */ 
        "  }\n"
        "  return r;\n"                            /* 14 */ 
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
        STRATA_CHECK_EQ(entry(), 14);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(narrowed_element_arithmetic_index_mutation_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    /* Same test, then the index variable moves: the fact must die. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[i + 1]?)\n"
        "  {\n"
        "    i = 2;\n"
        "    return rack[i + 1].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_index_spelling_mismatch_is_unproven)
{
    Arena arena; arena_init(&arena, 0);

    /* Equally-VALUED but differently-SPELLED indices are distinct keys:
       proving one must not prove the other (the compiler does not fold). */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 1;\n"
        "  if (rack[i + 1]?)\n"
        "  {\n"
        "    return rack[1 + i].dmg;\n"      /* "1 + i" != "i + 1" */ 
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_parenthesization_is_unambiguous)
{
    Arena arena; arena_init(&arena, 0);

    /* "(i+1)*2" and "i+1*2" evaluate differently; their spellings must
       differ so one can never prove the other. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  uint i = 0;\n"
        "  if (rack[(i + 1) * 2]?)\n"
        "  {\n"
        "    return rack[i + 1 * 2].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_length_index_runs_and_push_invalidates)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 5 } };\n"
        "  int r = 0;\n"
        "  if (rack[rack.length - 1]?)\n"
        "  {\n"
        "    r += rack[rack.length - 1].dmg;\n"    /* 5 */ 
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

STRATA_TEST(narrowed_element_length_index_push_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    /* push grows the length: "[rack.length - 1]" now names a different
       element, so the fact must die even though push preserves values. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 5 } };\n"
        "  if (rack[rack.length - 1]?)\n"
        "  {\n"
        "    array_push(rack, Weapon { .dmg = 9 });\n"
        "    return rack[rack.length - 1].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_call_index_is_refused)
{
    Arena arena; arena_init(&arena, 0);

    /* Calls have side effects and no stable spelling: never provable. This
       includes calls with arguments (`foo[some_call(5)]`) - the two
       evaluations may return different indices. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int f() { return 0; }\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  if (rack[f()]?)\n"
        "  {\n"
        "    return rack[f()].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "not a constant or a trackable variable", &arena));

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "uint pick(uint n) { return n; }\n"
        "int entry() {\n"
        "  Weapon?[] rack = { Weapon { .dmg = 3 } };\n"
        "  if (rack[pick(1)]?)\n"
        "  {\n"
        "    return rack[pick(1)].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "not a constant or a trackable variable", &arena));

    arena_free(&arena);
}

STRATA_TEST(narrowed_element_call_index_materializes_into_local)
{
    /* The blessed idiom: evaluate the call ONCE into a local - the local
       is dependency-tracked, so test and use provably agree. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "uint pick(uint n) { return n; }\n"
        "int entry() {\n"
        "  Weapon?[] rack;\n"
        "  array_push(rack, Weapon { .dmg = 3 });\n"
        "  array_push(rack, Weapon { .dmg = 7 });\n"
        "  uint k = pick(1);\n"
        "  int r = 0;\n"
        "  if (rack[k]?)\n"
        "  {\n"
        "    r += rack[k].dmg;\n"          /* rack[1] -> 7 */ 
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
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
}

/* ---- Dependent-array indexing (`foo.items[0].nested[j]`) ------------------
   Keys compose through member+index chains ("foo.items[0].nested[1]"), so
   narrowing is precise at EVERY index level, dependencies track every
   variable the full spelling mentions, and member reads through optional
   elements still require their own proof. */

STRATA_TEST(nested_dependent_array_indexing_runs)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  Cell? empty2;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty, Cell { .v = 3 } } );\n"   /* nested[1] empty */
        "  ^Item b = Item( { empty2 } );\n"                                      /* one empty element */
        "  ^Foo foo = Foo( { a, b } );\n"                                        /* items[0]=a, [1]=b */
        "  int sum = 0;\n"
        "  if (foo.items[0].nested[0]?) { sum += foo.items[0].nested[0].v; }\n" /* 1 */
        "  if (foo.items[0].nested[1]?) { sum += 1000; } else { sum += 10; }\n" /* empty -> 10 */
        "  if (foo.items[0].nested[2]?) { sum += foo.items[0].nested[2].v; }\n" /* 3 */
        "  if (foo.items[1].nested[0]?) { sum += 5000; } else { sum += 100; }\n"/* b[0] empty -> 100 */
        "  return sum;\n"                                                       /* 114 */
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
        STRATA_CHECK_EQ(entry(), 114);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(nested_dependent_array_index_precise_at_both_levels)
{
    Arena arena; arena_init(&arena, 0);

    /* Proving items[0].nested[0] proves neither the other ITEM... */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty } );\n"
        "  ^Item b = Item( { } );\n"
        "  ^Foo foo = Foo( { a, b } );\n"
        "  if (foo.items[0].nested[0]?)\n"
        "  {\n"
        "    return foo.items[1].nested[0].v;\n"   /* different item: unproven */
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'foo.items[1].nested[0]' has not been blessed", &arena));

    /* ...nor the other ELEMENT of the same item's nested array. */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty, Cell { .v = 3 } } );\n"
        "  ^Item b = Item( { } );\n"
        "  ^Foo foo = Foo( { a, b } );\n"
        "  if (foo.items[0].nested[0]?)\n"
        "  {\n"
        "    return foo.items[0].nested[2].v;\n"   /* different element: unproven */
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'foo.items[0].nested[2]' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(nested_dependent_array_index_var_mutation_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    /* Mutating the OUTER index variable kills the fact... */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty } );\n"
        "  ^Foo foo = Foo( { a } );\n"
        "  uint i = 0;\n"
        "  uint j = 0;\n"
        "  if (foo.items[i].nested[j]?)\n"
        "  {\n"
        "    i = 1;\n"
        "    return foo.items[i].nested[j].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    /* ...and so does mutating the INNER one. */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty } );\n"
        "  ^Foo foo = Foo( { a } );\n"
        "  uint i = 0;\n"
        "  uint j = 0;\n"
        "  if (foo.items[i].nested[j]?)\n"
        "  {\n"
        "    j = 1;\n"
        "    return foo.items[i].nested[j].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(nested_dependent_array_variable_indices_run)
{
    /* The mirror positive: both indices stable, test and use agree. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  ^Item a = Item( { Cell { .v = 1 }, empty, Cell { .v = 3 } } );\n"
        "  ^Item b = Item( { Cell { .v = 7 } } );\n"
        "  ^Foo foo = Foo( { a, b } );\n"
        "  int sum = 0;\n"
        "  for (uint i = 0; i < foo.items.length; i++)\n"
        "  {\n"
        "    for (uint j = 0; j < foo.items[i].nested.length; j++)\n"
        "    {\n"
        "      if (foo.items[i].nested[j]?) { sum += foo.items[i].nested[j].v; }\n"   /* 1+3+7 */
        "    }\n"
        "  }\n"
        "  return sum;\n"       /* 11 */
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
    }

    strataJitDestroy(jit);
}

/* ---- Index-as-index (`foo[other[bar]]`) and 2D arrays ---------------------
   A nested index is spelled recursively ("foo[other[bar]]"), so test and
   use agree; deps track every variable mentioned (bar) and every array
   whose ELEMENTS feed the spelling (other) - element writes, rebinds,
   builtins and ref passes all invalidate. 2D indexing composes the same
   machinery ("grid[i][j]"). */

STRATA_TEST(nested_index_as_index_runs)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] foo;\n"
        "  array_push(foo, Weapon { .dmg = 3 });\n"
        "  array_push(foo, Weapon { .dmg = 7 });\n"
        "  uint[] other = {1, 0};\n"
        "  uint bar = 0;\n"
        "  int r = 0;\n"
        "  if (foo[other[bar]]?)\n"
        "  {\n"
        "    r += foo[other[bar]].dmg;\n"      /* foo[1] -> 7 */ 
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
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(nested_index_as_index_source_write_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    /* Writing an element of the INDEX SOURCE array changes what
        foo[other[bar]] evaluates to: the fact must die. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] foo = { Weapon { .dmg = 3 }, Weapon { .dmg = 7 } };\n"
        "  uint[] other = {1, 0};\n"
        "  uint bar = 0;\n"
        "  if (foo[other[bar]]?)\n"
        "  {\n"
        "    other[0] = 0;\n"
        "    return foo[other[bar]].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(nested_index_as_index_var_mutation_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "int entry() {\n"
        "  Weapon?[] foo = { Weapon { .dmg = 3 }, Weapon { .dmg = 7 } };\n"
        "  uint[] other = {1, 0};\n"
        "  uint bar = 0;\n"
        "  if (foo[other[bar]]?)\n"
        "  {\n"
        "    bar = 1;\n"
        "    return foo[other[bar]].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(nested_index_as_index_ref_source_pass_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    /* Handing the index source to a callee by ref lets it rewrite the
        mapping: dependent facts die. */
    STRATA_CHECK(OptDerefRejected(
        "struct Weapon { int dmg; };\n"
        "void shuffle(ref uint[] a) { a[0] = 0; }\n"
        "int entry() {\n"
        "  Weapon?[] foo = { Weapon { .dmg = 3 }, Weapon { .dmg = 7 } };\n"
        "  uint[] other = {1, 0};\n"
        "  uint bar = 0;\n"
        "  if (foo[other[bar]]?)\n"
        "  {\n"
        "    shuffle(other);\n"
        "    return foo[other[bar]].dmg;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(two_d_optional_elements_narrow_per_element)
{
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  Cell?[][] grid = { { Cell { .v = 1 }, empty, Cell { .v = 3 } }, { Cell { .v = 7 } } };\n"
        "  int sum = 0;\n"
        "  for (uint i = 0; i < grid.length; i++)\n"
        "  {\n"
        "    for (uint j = 0; j < grid[i].length; j++)\n"
        "    {\n"
        "      if (grid[i][j]?) { sum += grid[i][j].v; }\n"      /* 1 + 3 + 7 */
        "    }\n"
        "  }\n"
        "  return sum;\n"                                        /* 11 */
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
    }

    strataJitDestroy(jit);
}

STRATA_TEST(two_d_index_precision_at_both_levels)
{
    Arena arena; arena_init(&arena, 0);

    /* Proving grid[0][0] proves neither the other ROW... */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  Cell?[][] grid = { { Cell { .v = 1 } }, { Cell { .v = 2 } } };\n"
        "  if (grid[0][0]?)\n"
        "  {\n"
        "    return grid[1][0].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'grid[1][0]' has not been blessed", &arena));

    /* ...nor the other COLUMN of the same row. */
    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  Cell?[][] grid = { { Cell { .v = 1 }, empty, Cell { .v = 3 } } };\n"
        "  if (grid[0][0]?)\n"
        "  {\n"
        "    return grid[0][2].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'grid[0][2]' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(two_d_index_var_mutation_invalidates)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  Cell? empty;\n"
        "  Cell?[][] grid = { { Cell { .v = 1 } }, { Cell { .v = 2 } } };\n"
        "  uint i = 0;\n"
        "  uint j = 0;\n"
        "  if (grid[i][j]?)\n"
        "  {\n"
        "    j = 0;\n"                     /* even a same-valued rebind kills it */
        "    i = 1;\n"
        "    return grid[i][j].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(nested_dependent_array_element_move_poisons_source)
{
    /* Moving elements through a braced ctor array arg (`Foo({a, b})`) is a
       real move: the source boxes must read as moved afterward. The arg's
       element type is inferred late, so this exercises the move marking
       that runs at inference time. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "struct Foo { ^Item[] items; };\n"
        "int entry() {\n"
        "  ^Item a = Item( { } );\n"
        "  ^Foo foo = Foo( { a } );\n"
        "  return (int)a.nested.length;\n"      /* a was moved into the array */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'a' used after move"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(nested_member_through_optional_element_requires_proof)
{
    /* When the ELEMENT itself is optional (`Item?[]`), reaching its member
       needs the element proven first - the `?` test's ancestor chain walks
       through index nodes. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int v; };\n"
        "struct Item { Cell?[] nested; };\n"
        "int entry() {\n"
        "  Item?[] items;\n"
        "  ^Item a = Item( { Cell { .v = 2 } } );\n"
        "  array_push(items, a);\n"
        "  if (items[1].nested[0]?)\n"      /* items[1] itself unproven! */
        "  {\n"
        "    return items[1].nested[0].v;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'items[1]' has not been blessed", &arena));

    arena_free(&arena);

    /* The mirror positive: prove the element, then the inner element. */
    {
        const char* err = NULL;
        StrataJit* jit = CompileOpt(
            "struct Cell { int v; };\n"
            "struct Item { Cell?[] nested; };\n"
            "int entry() {\n"
            "  Item?[] items;\n"
            "  ^Item a = Item( { Cell { .v = 2 } } );\n"
            "  array_push(items, a);\n"
            "  int r = 0;\n"
            "  if (items[0]?)\n"
            "  {\n"
            "    if (items[0].nested[0]?)\n"
            "    {\n"
            "      r += items[0].nested[0].v;\n"   /* 2 */ 
            "    }\n"
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
            STRATA_CHECK_EQ(entry(), 2);
        }

        strataJitDestroy(jit);
    }
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
        "has not been blessed", &arena));

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
        "has not been blessed", &arena));

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
        "'s' has not been blessed", &arena));

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

/* ---- Rebinding a `T?` ------------------------------------------------------
   Every `=` into a `T?` rebinds the whole slot: the old blessing dies, and a
   new one exists only when the assigned value is itself provably non-empty
   (a non-optional source, or an optional path already blessed). Moving OUT
   of a `T?` leaves the source empty - never a use-after-move poison. */

STRATA_TEST(optional_rebind_from_unblessed_drops_fact)
{
    /* The readme_demo walkthrough: after `cur = cur.next` the condition's
       blessing is gone - reads through `cur` are rejected again. */
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct Cell { int n; Cell? next; };\n"
        "int entry() {\n"
        "  ^Cell a = Cell { .n = 1 };\n"
        "  Cell? cur = a;\n"
        "  while (cur?)\n"
        "  {\n"
        "    cur = cur.next;\n"
        "    return cur.n;\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        "'cur' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_decl_from_unblessed_source_is_not_blessed)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct W { int d; };\n"
        "int entry() {\n"
        "  W? a;\n"
        "  W? b = a;\n"
        "  return b.d;\n"
        "}\n",
        "'b' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_field_rebind_from_unblessed_drops_fact)
{
    Arena arena; arena_init(&arena, 0);

    STRATA_CHECK(OptDerefRejected(
        "struct M { int v; };\n"
        "struct W { M? m; };\n"
        "int entry() {\n"
        "  ^W w = W {};\n"
        "  M? x;\n"
        "  w.m = x;\n"
        "  return w.m.v;\n"
        "}\n",
        "'w.m' has not been blessed", &arena));

    arena_free(&arena);
}

STRATA_TEST(optional_rebind_from_blessed_source_keeps_fact)
{
    /* A blessed source transfers its proof: the lookahead advance stays
       legal because `cur = cur.next` happens under `if (cur.next?)`. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct Cell { int n; Cell? next; };\n"
        "int entry() {\n"
        "  ^Cell b = Cell { .n = 2 };\n"
        "  ^Cell a = Cell { .n = 1, .next = b };\n"
        "  Cell? cur = a;\n"
        "  if (cur.next?)\n"
        "  {\n"
        "    cur = cur.next;\n"
        "    return cur.n;\n"
        "  }\n"
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
        STRATA_CHECK_EQ(entry(), 2);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(optional_move_out_leaves_source_empty_not_moved)
{
    /* `W? b = a;` moves the value: `a` tests empty afterwards (no poison),
       and `b` inherits `a`'s proof so it is readable without a re-test. */
    const char* err = NULL;
    StrataJit* jit = CompileOpt(
        "struct W { int d; };\n"
        "int entry() {\n"
        "  W? a = W { .d = 1 };\n"
        "  W? b = a;\n"
        "  if (a?) { return 10; }\n"
        "  if (b?) { return b.d * 100; }\n"
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
        STRATA_CHECK_EQ(entry(), 100);
    }

    strataJitDestroy(jit);
}
