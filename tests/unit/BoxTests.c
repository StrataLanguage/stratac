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

STRATA_TEST(box_global_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { int x; };\nbox<V> g = V { .x = 1 };\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
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

STRATA_TEST(box_parameter_is_an_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve("struct V { int x; };\nint take(box<V> v) { return v.x; }\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
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
