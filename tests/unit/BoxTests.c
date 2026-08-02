#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

static StrataJit* CompileBox(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "box", err);
    strataCompilerDestroy(c);
    return jit;
}

STRATA_TEST(box_allocates_and_reads_struct_fields)
{
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Vec3 { float x; float y; float z; };\n"
        "float entry() {\n"
        "  box<Vec3> v = Vec3 { .x = 1.0, .y = 2.0, .z = 3.0 };\n"
        "  v.x = 10.0;\n"
        "  return v.x + v.y + v.z;\n"          /* 10 + 2 + 3 = 15 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    float (*entry)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        float r = entry();
        STRATA_CHECK(r > 14.9f && r < 15.1f);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_in_loop_does_not_crash)
{
    /* A box declared inside a loop body is freed each iteration (block scope). */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  int sum = 0;\n"
        "  for (int i = 0; i < 100; i++) {\n"
        "    box<Cell> c = Cell { .v = i };\n"
        "    sum += c.v;\n"
        "  }\n"
        "  return sum;\n"                       /* 0+1+...+99 = 4950 */
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
        STRATA_CHECK_EQ(entry(), 4950);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_returned_as_inner_struct_type_copies_value)
{
    /* Returning a box<Struct> by identifier from a function declared to
       return the plain struct (not the box) copies the value out before
       the box is freed - a read, not a move. Only sound because Struct
       here is non-owning (no box<T> fields), so a bitwise copy is safe. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; int w; };\n"
        "Cell make_cell() {\n"
        "  box<Cell> c = Cell { .v = 40, .w = 2 };\n"
        "  return c;\n"
        "}\n"
        "int entry() {\n"
        "  Cell c = make_cell();\n"
        "  return c.v + c.w;\n"     /* 40 + 2 = 42 */
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

STRATA_TEST(box_returned_struct_literal_boxes_at_return)
{
    /* `return Cell{...};` from a box<Cell>-returning function boxes the
       literal at the return site - no local box var needed first. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> make_cell(int n) { return Cell { .v = n }; }\n"
        "int entry() { box<Cell> c = make_cell(9); return c.v; }\n",
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

STRATA_TEST(box_returned_anonymous_struct_literal_boxes_at_return)
{
    /* `return { .v = n };` infers Cell (the box's inner type, not
       "box<Cell>") for the anonymous literal, then boxes it. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> make_cell(int n) { return { .v = n }; }\n"
        "int entry() { box<Cell> c = make_cell(13); return c.v; }\n",
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
        STRATA_CHECK_EQ(entry(), 13);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_global_with_valid_init_reads_and_mutates)
{
    /* A box global boxed from a value initializer: readable and field-
       mutable from any function, and freed automatically on JIT teardown. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> g = Cell { .v = 5 };\n"
        "void bump() { g.v = g.v + 1; }\n"
        "int entry() {\n"
        "  bump();\n"
        "  bump();\n"
        "  return g.v;\n"          /* 5 + 1 + 1 = 7 */
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

STRATA_TEST(box_global_with_call_init_reads)
{
    /* A box global boxed from a box-returning call. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> make() { box<Cell> c = Cell { .v = 41 }; return c; }\n"
        "box<Cell> g = make();\n"
        "int entry() { return g.v + 1; }\n",
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

STRATA_TEST(box_global_scalar_value_used_in_arithmetic)
{
    /* A box<int> global's value is read directly in an arithmetic
       expression, not just via a bare `return g;` - it must be dereferenced
       rather than treated as a pointer, and never moved. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "box<int> i = 9;\n"
        "int entry() {\n"
        "  int a = 3;\n"
        "  return a + i;\n"   /* 3 + 9 = 12 */
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
        STRATA_CHECK_EQ(entry(), 12);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_global_scalar_bare_return_reads_value)
{
    /* `return i;` from a function whose return type is the boxed scalar's
       inner type (not box<int> itself) reads the value - it is not a
       move, so it's allowed even though 'i' is a box global. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "box<int> i = 41;\n"
        "int entry() { return i; }\n",
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
        STRATA_CHECK_EQ(entry(), 41);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_global_ref_param_borrows)
{
    /* Passing a box global to a 'ref' parameter borrows it: legal, and the
       global is still readable/live afterward. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> g = Cell { .v = 6 };\n"
        "int read(ref box<Cell> c) { return c.v; }\n"
        "int entry() { return read(g) * g.v; }\n"   /* 6 * 6 = 36 */
        ,
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
        STRATA_CHECK_EQ(entry(), 36);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_global_uninitialized_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { int x; };\nbox<V> g;\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_global_move_init_from_another_global_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> a = V { .x = 1 };\n"
        "box<V> b = a;\n",          /* error: moving from a global */
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_global_reassignment_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> g = V { .x = 1 };\n"
        "box<V> other = V { .x = 2 };\n"
        "void set() { g = other; }\n",   /* error: box global cannot be reassigned */
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot be reassigned"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_global_moved_into_local_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> g = V { .x = 1 };\n"
        "int entry() { box<V> local = g; return local.x; }\n",   /* error: moving global */
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "box global 'g' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_global_passed_to_owned_param_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> g = V { .x = 1 };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() { return take(g); }\n",   /* error: moving global into owned param */
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "box global 'g' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_global_returned_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> g = V { .x = 1 };\n"
        "box<V> give() { return g; }\n",   /* error: moving global out via return */
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "box global 'g' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_owning_field_linked_list)
{
    /* A recursive owning struct: box<Node> with a box<Node> next field.
       Building the list moves boxes into fields; dropping the head frees the
       whole chain recursively. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Node { int v; box<Node> next; };\n"
        "box<Node> build() {\n"
        "  box<Node> c = Node { .v = 3 };\n"
        "  box<Node> b = Node { .v = 2, .next = c };\n"
        "  box<Node> a = Node { .v = 1, .next = b };\n"
        "  return a;\n"
        "}\n"
        "int entry() {\n"
        "  box<Node> head = build();\n"
        "  return head.v + head.next.v + head.next.next.v;\n"
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
        STRATA_CHECK_EQ(entry(), 6);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_use_after_struct_field_move_is_an_error)
{
    /* Putting a box into a box-typed struct field moves it out (the struct
       now owns it, and codegen nulls the source) - using the source
       variable afterward must be a compile error, not a runtime crash. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Pistol { int ammo; };\n"
        "struct Holder { box<Pistol> p; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 1 };\n"
        "  box<Holder> h = Holder { p };\n"    /* p moved into h.p */
        "  return p.ammo;\n"                    /* error: use of moved box 'p' */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'p'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_anonymous_struct_literal_infers_box_inner_type)
{
    /* `box<Holder> h = { p };` infers Holder (the box's inner type, not
       "box<Holder>") for the anonymous literal, same as a named literal. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Pistol { int ammo; };\n"
        "struct Holder { box<Pistol> p; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 42 };\n"
        "  box<Holder> h = { p };\n"
        "  return h.p.ammo;\n"
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

STRATA_TEST(box_field_wrong_box_inner_type_is_an_error)
{
    /* box<Any> and box<Pistol> are unrelated - a struct-init field value's
       type was never checked against the field's declared type at all
       (only the field name/position), so this silently passed sema before
       and miscompiled a mismatched box pointer into the field. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Pistol { int ammo; };\n"
        "struct Any;\n"
        "struct Holder { box<Any> gun; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 1 };\n"
        "  box<Holder> h = { p };\n"    /* error: box<Pistol> into box<Any> field */
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "field 'gun'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(struct_field_wrong_scalar_type_is_an_error)
{
    /* Same gap, no box involved: a plain struct field's value type was
       never checked either. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Inner { int x; };\n"
        "struct Outer { Inner i; };\n"
        "struct Other { float z; };\n"
        "int entry() {\n"
        "  Outer o = Outer { .i = Other { .z = 1.0 } };\n"   /* error */
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "field 'i'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_owning_struct_field_allowed)
{
    /* box<T> fields are now allowed (the struct becomes owning). */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { int x; };\nstruct W { box<V> v; };\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_factory_returns_and_caller_owns)
{
    /* A box returned from a function is moved out; the caller owns and frees it. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "box<Cell> make(int n) {\n"
        "  box<Cell> c = Cell { .v = n };\n"
        "  c.v = c.v * 2;\n"
        "  return c;\n"
        "}\n"
        "int entry() { box<Cell> w = make(21); return w.v; }\n",
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

STRATA_TEST(box_owned_param_consumes_callers_box)
{
    /* An owned (non-ref) box parameter takes ownership: the caller's box is
       moved (nulled), and the callee frees it at return. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int consume(box<Cell> c) { return c.v; }\n"
        "int entry() {\n"
        "  box<Cell> a = Cell { .v = 9 };\n"
        "  int r = consume(a);\n"          /* a moved */
        "  return r;\n"
        "}\n",
        &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err?err:"(none)"); strataFree((char*)err); return; }
    int (*entry)(void) = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 9);
    strataJitDestroy(jit);
}

STRATA_TEST(box_ref_param_borrows_callers_box)
{
    /* A `ref box<T>` parameter is a borrow: the callee reads the box through
       the reference but the caller still owns and frees it. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int read(ref box<Cell> c) { return c.v; }\n"
        "int entry() {\n"
        "  box<Cell> a = Cell { .v = 7 };\n"
        "  int r = read(a);\n"             /* borrow – a stays alive */
        "  return r * a.v;\n"               /* 7 * 7 = 49 */
        "}\n",
        &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err?err:"(none)"); strataFree((char*)err); return; }
    int (*entry)(void) = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 49);
    strataJitDestroy(jit);
}

STRATA_TEST(box_moved_unconditionally_every_loop_iteration_is_error)
{
    /* A box moved into an owned param on every iteration of a loop is only
       valid for iteration 1 - by iteration 2 it's already freed. This must
       be caught statically even though a single top-to-bottom walk of the
       loop body alone can't see it (the use precedes the move in the same
       textual pass); it only shows up once the moved-state a prior
       iteration leaves behind is carried into the next one. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 1 };\n"
        "  for (int i = 0; i < 10; i++) {\n"
        "    take(a);\n"                      /* fine on iteration 1, freed after */
        "  }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'a'"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_fresh_local_per_iteration_moved_in_loop_is_allowed)
{
    /* A box declared fresh inside the loop body (not loop-carried) may be
       freely moved each iteration - it's a brand new binding every time,
       not the same live box surviving into the next iteration. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct V { int x; };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() {\n"
        "  int sum = 0;\n"
        "  for (int i = 0; i < 5; i++) {\n"
        "    box<V> local = V { .x = i };\n"
        "    sum += take(local);\n"
        "  }\n"
        "  return sum;\n"     /* 0+1+2+3+4 = 10 */
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

STRATA_TEST(box_moved_in_one_if_branch_used_only_in_other_branch_is_allowed)
{
    /* then/else are mutually exclusive: moving a box in one branch and
       using it only in the other is safe, since at most one branch runs. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct V { int x; };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 7 };\n"
        "  bool cond = false;\n"
        "  if (cond) {\n"
        "    return take(a);\n"
        "  } else {\n"
        "    return a.x;\n"
        "  }\n"
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

STRATA_TEST(box_moved_in_one_if_branch_used_unconditionally_after_is_error)
{
    /* Moved in only one branch, but used unconditionally after the whole
       if - unsound, since the taken branch isn't known statically. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 1 };\n"
        "  bool cond = false;\n"
        "  if (cond) {\n"
        "    take(a);\n"
        "  }\n"
        "  return a.x;\n"                     /* error: maybe moved above */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'a'"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_owned_param_use_after_call_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "int take(box<V> v) { return v.x; }\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 1 };\n"
        "  int r = take(a);\n"             /* a moved */
        "  return a.x;\n"                   /* error: use of moved box 'a' */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'a'"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_reassign_moves_ownership)
{
    /* `a = b` frees a's old box and moves b's pointer into a (b is nulled). */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  box<Cell> a = Cell { .v = 10 };\n"
        "  box<Cell> b = Cell { .v = 32 };\n"
        "  a = b;\n"
        "  return a.v;\n"
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
        STRATA_CHECK_EQ(entry(), 32);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_uninitialized_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { int x; };\nint entry() { box<V> v; return 0; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_use_after_vardecl_move_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 1 };\n"
        "  box<V> b = a;\n"          /* a moved into b */
        "  return a.x;\n"            /* error: use of moved box 'a' */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'a'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_use_after_assign_move_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "int entry() {\n"
        "  box<V> a = V { .x = 1 };\n"
        "  box<V> b = V { .x = 2 };\n"
        "  a = b;\n"                  /* b moved into a */
        "  return b.x;\n"             /* error: use of moved box 'b' */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "use of moved box 'b'"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_reassign_after_move_revalidates)
{
    /* After a is moved, reassigning it makes it usable again. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct V { int x; };\n"
        "box<V> make(int n) { box<V> v = V { .x = n }; return v; }\n"
        "int entry() {\n"
        "  box<V> a = make(1);\n"
        "  box<V> b = a;\n"          /* a moved */
        "  a = make(7);\n"           /* a re-Live */
        "  return a.x + b.x;\n"      /* ok: both live -> 7 + 1 = 8 */
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
        STRATA_CHECK_EQ(entry(), 8);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_coerced_to_bare_param_borrows)
{
    /* Passing box<T> where T is expected borrows the heap pointer — the
       box survives the call (not consumed). */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int read_bare(const Cell c) { return c.v; }\n"
        "int entry() {\n"
        "  box<Cell> a = Cell { .v = 5 };\n"
        "  int r = read_bare(a);\n"      /* borrow: a stays alive */
        "  return r + a.v;\n"             /* 5 + 5 = 10 */
        "}\n",
        &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err?err:"(none)"); strataFree((char*)err); return; }
    int (*entry)(void) = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 10);
    strataJitDestroy(jit);
}

STRATA_TEST(box_coerced_to_bare_return_unboxes)
{
    /* Returning box<T> from a T-returning function loads the value out and
       frees the box at the function's return. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "Cell extract() {\n"
        "  box<Cell> c = Cell { .v = 42 };\n"
        "  return c;\n"                   /* unbox: load value, free box */
        "}\n"
        "int entry() {\n"
        "  Cell r = extract();\n"
        "  return r.v;\n"                 /* 42 */
        "}\n",
        &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err?err:"(none)"); strataFree((char*)err); return; }
    int (*entry)(void) = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 42);
    strataJitDestroy(jit);
}

STRATA_TEST(box_coerced_to_bare_vardecl_copies)
{
    /* T v = boxVal; copies the pointee — the box is not consumed. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Cell { int v; };\n"
        "int entry() {\n"
        "  box<Cell> a = Cell { .v = 7 };\n"
        "  Cell copy = a;\n"             /* deref-copy: copy = *a */
        "  copy.v = 99;\n"               /* mutate the copy */
        "  return a.v;\n"                /* a unchanged: 7 */
        "}\n",
        &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err?err:"(none)"); strataFree((char*)err); return; }
    int (*entry)(void) = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 7);
    strataJitDestroy(jit);
}
