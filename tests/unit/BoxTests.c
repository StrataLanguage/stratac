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

STRATA_TEST(box_ref_param_passed_to_owned_param_is_an_error)
{
    /* `ref box<T>` is a borrow of the CALLER's box - the callee doesn't own
       it, so passing it on to a consuming (owned, non-ref) parameter must
       be rejected. Without this, the callee frees/nulls the caller's own
       box out from under it with no way for the caller's sema to notice,
       since move-tracking is per-function: the caller still thinks its box
       is live after the call returns, and dereferencing it crashes. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "void drop(box<V> v) {}\n"
        "void mutate(ref box<V> inBox) {\n"
        "  drop(inBox);\n"    /* error: inBox is borrowed, not owned */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'inBox' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_ref_param_moved_into_local_is_an_error)
{
    /* Same rule, different move context: moving a ref box<T> param into a
       fresh local also isn't a real move - the callee still doesn't own
       the source. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "void mutate(ref box<V> inBox) {\n"
        "  box<V> stolen = inBox;\n"   /* error */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'inBox' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_ref_param_returned_is_an_error)
{
    /* Same rule again: returning a ref box<T> param as box<T> would move
       the caller's box out through the return value. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct V { int x; };\n"
        "box<V> steal(ref box<V> inBox) {\n"
        "  return inBox;\n"   /* error */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'inBox' cannot be moved"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_ref_param_rebind_via_box_value_is_an_error)
{
    /* `inBox = newBox;` (RHS is itself a box<T>) looks like a move-assign
       target, so sema tries the real box-reassign path - and rejects it,
       since `inBox` is a `ref box<T>` borrow, not something this function
       owns to rebind. Silently allowing this would rebind the CALLER's
       variable out from under it via the shared slot, invisibly to the
       caller's own move-tracking. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Vec3 { float x; };\n"
        "void mutate_box(ref box<Vec3> inBox) {\n"
        "  box<Vec3> newBox = Vec3 { .x = 100.0 };\n"
        "  inBox = newBox;\n"    /* error: rebind through a ref */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'inBox' cannot be reassigned"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_ref_param_inner_value_assign_mutates_in_place)
{
    /* The legitimate way to write through a ref box<T>: assign its INNER
       value (a plain Vec3, not box<Vec3>). This overwrites the contents of
       whatever box the caller already owns, in place - the caller's box
       keeps the same identity, only its data changes. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Vec3 { float x; float y; float z; };\n"
        "box<Vec3> make_vec3(float x) { box<Vec3> v = Vec3 { .x = x }; return v; }\n"
        "void mutate_box(ref box<Vec3> inBox) {\n"
        "  box<Vec3> newBox = Vec3 { .x = 100.0 };\n"
        "  Vec3 newBoxVal = newBox;\n"   /* read newBox's value (not a move) */
        "  inBox = newBoxVal;\n"          /* content-assign, in place */
        "}\n"
        "int entry() {\n"
        "  box<Vec3> w = make_vec3(0.0);\n"
        "  mutate_box(w);\n"
        "  return (int)w.x;\n"    /* 100 */
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

STRATA_TEST(box_value_assigned_into_plain_ref_target_derefs)
{
    /* Assigning a box<T> value directly into a plain (non-box) `ref T`
       target - not `ref box<T>` - reads through the box in place, same
       box<T> -> T coercion already used for var-decl inits/call args/
       returns. Was previously unvalidated by sema and unhandled by
       codegen, which emitted an invalid `Vec3 = Vec3*` C assignment. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Vec3 { float x; };\n"
        "void mutate(ref Vec3 inBox) {\n"
        "  box<Vec3> newBox = Vec3 { .x = 100.0 };\n"
        "  inBox = newBox;\n"     /* box<Vec3> -> Vec3, in place */
        "}\n"
        "int entry() {\n"
        "  Vec3 w = Vec3 { .x = 0.0 };\n"
        "  mutate(w);\n"
        "  return (int)w.x;\n"    /* 100 */
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

STRATA_TEST(box_value_assigned_into_mismatched_ref_target_is_an_error)
{
    /* box<Pistol> assigned into a plain (non-box) `ref Vec3` target - the
       inner types don't match, so this is a genuine mismatch and must
       still be rejected, not silently passed through like before. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Pistol { int ammo; };\n"
        "struct Vec3 { float x; };\n"
        "void mutate(ref Vec3 target) {\n"
        "  box<Pistol> p = Pistol { .ammo = 1 };\n"
        "  target = p;\n"    /* error: box<Pistol> into ref Vec3 */
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot assign"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_ref_param_reborrow_still_allowed)
{
    /* Passing a ref box<T> param on to ANOTHER ref box<T> param is a
       re-borrow, not a move - must still be allowed. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct V { int x; };\n"
        "int read_it(ref box<V> v) { return v.x; }\n"
        "int reborrow(ref box<V> inBox) { return read_it(inBox); }\n"
        "int entry() {\n"
        "  box<V> v = V { .x = 9 };\n"
        "  return reborrow(v);\n"
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

STRATA_TEST(box_compound_assign_mutates_contents_in_place)
{
    /* `val -= amt;` on a box<int> target mutates the boxed value in place -
       not a move - so it works through a `ref box<T>` param. It was
       previously misclassified as a full box-move, requiring the RHS to
       itself be a box<int>. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "void sub(ref box<int> val, int amt) { val -= amt; }\n"
        "int entry() {\n"
        "  box<int> x = 15;\n"
        "  sub(x, 25);\n"
        "  return x;\n"     /* 15 - 25 = -10 */
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
        STRATA_CHECK_EQ(entry(), -10);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_plain_assign_of_inner_value_mutates_contents)
{
    /* `x = 5;` where x is box<int> and 5 is a plain int (not box<int>)
       also mutates in place - only `=` with a matching box<T> value is a
       move. It was previously always treated as a move, rejecting any
       non-box RHS outright. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "void reassign(box<int> x) { x = 5; }\n"
        "int entry() {\n"
        "  box<int> x = 15;\n"
        "  int before = x;\n"
        "  reassign(x);\n"
        "  return before;\n"    /* captured before reassign runs -> 15 */
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
        STRATA_CHECK_EQ(entry(), 15);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_reassign_from_call_still_moves)
{
    /* `a = make(7);` (RHS is itself box<T>, from a call) must still be a
       real move-rebind, not misclassified as content-assign, since the
       value's type isn't known until the call is resolved. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct V { int x; };\n"
        "box<V> make(int n) { box<V> v = V { .x = n }; return v; }\n"
        "int entry() {\n"
        "  box<V> a = make(1);\n"
        "  a = make(7);\n"
        "  return a.x;\n"    /* 7 */
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

STRATA_TEST(box_passed_to_by_value_scalar_param_derefs)
{
    /* box<int> passed to a plain `int` param (a by-value, non-indirect
       param - handles hit the same path) must be dereferenced to its
       value. It was previously passed as the box's own heap pointer,
       which crashes/misbehaves for any real handle-typed param. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "int take(int x) { return x; }\n"
        "int entry() {\n"
        "  box<int> b = 41;\n"
        "  return take(b);\n"
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
        STRATA_CHECK_EQ(entry(), 41);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(box_returned_as_inner_struct_type_copies_value)
{
    /* Returning box<Struct> as plain Struct copies the value, then frees the box. */
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
    /* `return Cell{...};` boxes the literal at the return site. */
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
    /* `return { .v = n };` infers Cell, not "box<Cell>", then boxes it. */
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
    /* A box global is readable/mutable from any function, freed at teardown. */
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
    /* A box<int> global's value is dereferenced in an expression, not just a bare return. */
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
    /* `return i;` reads the value (not box<int>), so it's not a move. */
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
    /* Passing a box global to 'ref' borrows it; still live afterward. */
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
    STRATA_CHECK(Contains(d, "g' cannot be moved as it is not owned because it is global"));

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
    STRATA_CHECK(Contains(d, "g' cannot be moved as it is not owned because it is global"));

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
    STRATA_CHECK(Contains(d, "g' cannot be moved as it is not owned because it is global"));

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
    STRATA_CHECK(Contains(d, "'p' used after move"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_anonymous_struct_literal_infers_box_inner_type)
{
    /* `box<Holder> h = { p };` infers Holder, not "box<Holder>". */
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
    /* box<Any> and box<Pistol> are unrelated - field types weren't checked before. */
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

STRATA_TEST(box_cast_via_opaque_marker_round_trips)
{
    /* box<Pistol> -> box<Any> -> box<Pistol>: allowed since Any is opaque.
       Each cast moves its source, so nothing is freed twice. */
    const char* err = NULL;
    StrataJit* jit = CompileBox(
        "struct Pistol { int ammo; };\n"
        "struct Any;\n"
        "struct Holder { box<Any> gun; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 100 };\n"
        "  box<Holder> holder = { (box<Any>)p };\n"
        "  box<Pistol> p2 = (box<Pistol>)holder.gun;\n"
        "  return p2.ammo;\n"
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

STRATA_TEST(box_cast_between_unrelated_concrete_structs_is_an_error)
{
    /* Neither side is opaque, so this stays rejected. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Pistol { int ammo; };\n"
        "struct Vec3 { float x; float y; float z; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 1 };\n"
        "  box<Vec3> v = (box<Vec3>)p;\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "invalid cast"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_field_extracted_twice_is_an_error)
{
    /* Extracting a box moves it out of the field; reading it again is an error. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Pistol { int ammo; };\n"
        "struct Any;\n"
        "struct Holder { box<Any> gun; };\n"
        "int entry() {\n"
        "  box<Pistol> p = Pistol { .ammo = 1 };\n"
        "  box<Holder> holder = { (box<Any>)p };\n"
        "  box<Pistol> p2 = (box<Pistol>)holder.gun;\n"
        "  box<Pistol> p3 = (box<Pistol>)holder.gun;\n"   /* error: gun moved already */
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "'holder.gun' used after move"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(struct_field_wrong_scalar_type_is_an_error)
{
    /* Same gap, no box involved. */
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
    /* Moving the box every iteration is only valid on iteration 1. */
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
    STRATA_CHECK(Contains(d, "'a' used after move"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(box_fresh_local_per_iteration_moved_in_loop_is_allowed)
{
    /* A box declared fresh each iteration is a new binding, movable freely. */
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
    /* Moving in one branch, using only in the other, is safe (mutually exclusive). */
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
    /* Moved in one branch, used unconditionally after - unsound. */
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
    STRATA_CHECK(Contains(d, "'a' used after move"));
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
    STRATA_CHECK(Contains(d, "'a' used after move"));
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
    STRATA_CHECK(Contains(d, "'a' used after move"));

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
    STRATA_CHECK(Contains(d, "'b' used after move"));

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
