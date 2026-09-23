/* CodegenRegressionTests.c — end-to-end JIT regressions for codegen bugs:
 * each test runs a small program whose result was wrong (or which crashed
 * the compiler) before the corresponding fix. */

#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <stdlib.h>

static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "cgreg", &err);

    if (err)
    {
        printf("  JIT failed: %s\n", err);
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);
    return jit;
}

/* Compiles `src` and returns entry()'s result, or -9999 on failure. */
static int RunEntry(const char* src)
{
    StrataJit* jit = CompileJit(src);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return -9999;
    }

    int result = -9999;
    int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(f != NULL);
    if (f)
    {
        result = f();
    }

    strataJitDestroy(jit);
    return result;
}

/* A `for` body that always ends in `continue` / `return` left for.update
   without a terminator (invalid IR; the continue case crashed codegen). */
STRATA_TEST(for_body_always_continues)
{
    STRATA_CHECK_EQ(RunEntry("int entry() {\n"
                             "  int s = 0;\n"
                             "  for (int i = 0; i < 5; i++) { s = s + i; continue; }\n"
                             "  return s;\n"
                             "}\n"),
                    10);
}

STRATA_TEST(for_body_always_returns)
{
    STRATA_CHECK_EQ(RunEntry("int f(int n) { for (int i = 0; i < n; i++) { return i + 7; } return 0; }\n"
                             "int entry() { return f(3) + f(0); }\n"),
                    7);
}

/* The pushed value was evaluated after the grow path freed the old buffer,
   so `array_push(a, a[0])` on a full array read freed memory. */
STRATA_TEST(array_push_value_reads_same_array)
{
    STRATA_CHECK_EQ(RunEntry("int entry() {\n"
                             "  int[] a = {1, 2, 3, 4};\n" /* full: first push grows */
                             "  array_push(a, a[0]);\n"
                             "  array_push(a, a[4] + 10);\n"
                             "  return a[4] * 100 + a[5];\n"
                             "}\n"),
                    111);
}

/* Mixed-width integer arithmetic truncated the wider operand to the
   narrower type (`int + byte` computed in i8). */
STRATA_TEST(mixed_width_int_arith_widens)
{
    STRATA_CHECK_EQ(RunEntry("int add(int x, byte c) { return x + c; }\n"
                             "int mul(int x, short c) { return x * c; }\n"
                             "int entry() { return (add(300, 1) - 300) + (mul(1000, 2) - 2000); }\n"),
                    1);
}

/* An owning global initialized from a call was never stored, leaving the
   context field as uninitialized heap (and freeing garbage at destroy). */
STRATA_TEST(owning_global_initialized_from_call)
{
    StrataJit* jit = CompileJit("int[] mk() { int[] r = {5, 6, 7}; return r; }\n"
                                "int[] ga = mk();\n"
                                "int get() { return ga[0] + ga[2] + (int)ga.length; }\n");
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return;
    }

    void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
    int (*get)(void*) = (int (*)(void*))strataJitGetFunction(jit, "get");

    STRATA_CHECK(create != NULL && destroy != NULL && get != NULL);
    if (create && destroy && get)
    {
        void* ctx = create();
        STRATA_CHECK_EQ(get(ctx), 15);
        destroy(ctx);
    }

    strataJitDestroy(jit);
}

/* ---- Allocation balance ----
   Runs entry() under counting alloc/free hooks. Only non-null frees count
   (drops emit a harmless strata_free(NULL) for moved-out slots). A leak
   shows as allocs > frees, a double free as frees > allocs. */

static int g_cgAllocs = 0;
static int g_cgFrees = 0;

static void* CgCountAlloc(unsigned long long n)
{
    g_cgAllocs++;
    return malloc((size_t)n);
}

static void CgCountFree(void* p)
{
    if (p)
    {
        g_cgFrees++;
    }
    free(p);
}

/* Compiles `src` with the counting allocator and returns entry()'s result
   (-9999 on failure); the balance is left in g_cgAllocs / g_cgFrees. */
static int RunEntryCounted(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, (void*)&CgCountAlloc, (void*)&CgCountFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "cgreg", &err);

    if (err)
    {
        printf("  JIT failed: %s\n", err);
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return -9999;
    }

    g_cgAllocs = 0;
    g_cgFrees = 0;

    int result = -9999;
    int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(f != NULL);
    if (f)
    {
        result = f();
    }

    strataJitDestroy(jit);
    return result;
}

/* An unbraced `if (c) break;` / `continue;` truncated the compile-time
   owning-locals list, so the loop body's box was never dropped on the
   iterations that did not break/continue. */
STRATA_TEST(unbraced_break_keeps_loop_body_drops)
{
    STRATA_CHECK_EQ(RunEntryCounted("int entry() {\n"
                                    "  int c = 6;\n"
                                    "  int s = 0;\n"
                                    "  while (c > 0) { ^int a = 7; c = c - 1; if (c == 3) break; s = s + a; }\n"
                                    "  return s;\n"
                                    "}\n"),
                    14);
    STRATA_CHECK_EQ(g_cgAllocs, 3);
    STRATA_CHECK_EQ(g_cgFrees, 3);
}

STRATA_TEST(unbraced_continue_keeps_loop_body_drops)
{
    STRATA_CHECK_EQ(RunEntryCounted("int entry() {\n"
                                    "  int c = 5;\n"
                                    "  int s = 0;\n"
                                    "  while (c > 0) { ^int a = c; c = c - 1; if (c == 2) continue; s = s + a; }\n"
                                    "  for (int i = 0; i < 3; i++) { ^int b = i; if (i == 1) continue; s = s + b; }\n"
                                    "  return s;\n" /* 5+4+2+1 + 0+2 */
                                    "}\n"),
                    14);
    STRATA_CHECK_EQ(g_cgAllocs, 8);
    STRATA_CHECK_EQ(g_cgFrees, 8);
}

/* A `for` init's owning local lived until the enclosing block's exit, so a
   loop nested in another loop re-initialized the slot every outer iteration
   without dropping the previous box. */
STRATA_TEST(for_init_owning_local_dropped_at_loop_exit)
{
    STRATA_CHECK_EQ(RunEntryCounted("int entry() {\n"
                                    "  int s = 0;\n"
                                    "  for (int j = 0; j < 3; j++) {\n"
                                    "    for (^int k = 10; k < 13; k = k + 1) { s = s + 1; }\n"
                                    "    for (^int m = 0; m < 5; m = m + 1) { if (m == 2) break; s = s + 1; }\n"
                                    "  }\n"
                                    "  for (int j = 0; j < 3; j++)\n" /* unbraced: no block end to hide it */
                                    "    for (^int k = 10; k < 12; k = k + 1) { s = s + 1; }\n"
                                    "  return s;\n" /* 3 * (3 + 2) + 3 * 2 */
                                    "}\n"),
                    21);
    STRATA_CHECK(g_cgAllocs > 0);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

/* The positional constructor only nulled box/string sources, so an array
   moved into a field was freed by both the local and the struct's drop. */
STRATA_TEST(positional_ctor_moves_array_field)
{
    STRATA_CHECK_EQ(RunEntryCounted("struct S { int[] v; };\n"
                                    "int entry() {\n"
                                    "  int[] a = {1, 2};\n"
                                    "  ^S s = S(a);\n"
                                    "  return s.v[0] + s.v[1];\n"
                                    "}\n"),
                    3);
    STRATA_CHECK(g_cgAllocs > 0);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

/* `o && k`, `o || k` and `!o` dereferenced the optional (null when empty)
   and tested its value; `if (o)` tests presence. All test presence now. */
STRATA_TEST(optional_truthiness_is_presence_everywhere)
{
    STRATA_CHECK_EQ(RunEntry("int entry() {\n"
                             "  int? e;\n"
                             "  int? z = 0;\n"
                             "  string? se;\n"
                             "  string? sp = \"\";\n"
                             "  bool k = true;\n"
                             "  int r = 0;\n"
                             "  if (e && k) { r = r + 1; }\n"    /* empty: false, no null deref */
                             "  if (!e) { r = r + 2; }\n"        /* empty: true */
                             "  if (e || k) { r = r + 4; }\n"    /* k */
                             "  if (z && k) { r = r + 8; }\n"    /* present (value 0): true */
                             "  if (!z) { r = r + 16; }\n"       /* present: false */
                             "  if (k && z) { r = r + 32; }\n"   /* rhs presence */
                             "  if (se || sp) { r = r + 64; }\n" /* string? presence */
                             "  if (se) { r = r + 128; }\n"
                             "  if (!sp) { r = r + 256; }\n"
                             "  return r;\n"
                             "}\n"),
                    2 + 4 + 8 + 32 + 64);
}

/* array_pop on an empty array read data[0xFFFFFFFF] and stored len =
   0xFFFFFFFF. It is bounds-checked like indexing now (the JIT reports
   through the panic handler and continues) and the length never underflows. */
static int g_cgPanics = 0;

static void CgPanicRecorder(const char* msg)
{
    (void)msg;
    g_cgPanics++;
}

STRATA_TEST(array_pop_empty_is_bounds_checked)
{
    g_cgPanics = 0;
    strataSetPanicHandler(CgPanicRecorder);

    int r = RunEntryCounted("int entry() {\n"
                            "  int[] a;\n"
                            "  int x = array_pop(a);\n"
                            "  array_push(a, 5);\n"
                            "  string[] s;\n"
                            "  string t = array_pop(s);\n"
                            "  return (int)a.length * 100 + a[0] * 10 + x + (int)s.length + (int)t.length;\n"
                            "}\n");

    strataSetPanicHandler(NULL);

    STRATA_CHECK_EQ(r, 150);
    STRATA_CHECK_EQ(g_cgPanics, 2);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

STRATA_TEST(array_pop_owning_element_moves_out)
{
    STRATA_CHECK_EQ(RunEntryCounted("int entry() {\n"
                                    "  string[] s = {\"ab\", \"cde\"};\n"
                                    "  string t = array_pop(s);\n"
                                    "  string u = array_pop(s);\n"
                                    "  return (int)t.length * 10 + (int)u.length + (int)s.length * 100;\n"
                                    "}\n"),
                    32);
    STRATA_CHECK(g_cgAllocs > 0);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

/* Owning call results borrowed inside a larger expression were never
   dropped: `mk().length`, `int v = mkb();`, `substring(mks(), ...)`,
   `mks() == "..."`, `mk()[i]`, in loop conditions and in return values.
   Results that are moved (bound, passed as an owned argument) must still be
   dropped exactly once. */
STRATA_TEST(owning_call_temporaries_are_dropped)
{
    STRATA_CHECK_EQ(RunEntryCounted("int[] mk() { int[] r = {1, 2, 3}; return r; }\n"
                                    "^int mkb() { ^int b = 41; return b; }\n"
                                    "string mks() { string s = \"hello\"; return s; }\n"
                                    "int unbox() { return mkb(); }\n"
                                    "int total(int[] a) { return a[0] + a[1] + a[2]; }\n"
                                    "int entry() {\n"
                                    "  int n = (int)mk().length;\n"          /* 3 */
                                    "  int v = mkb();\n"                     /* 41 */
                                    "  string t = substring(mks(), 1, 2);\n" /* "el" */
                                    "  int e = 0;\n"
                                    "  if (mks() == \"hello\") { e = 1; }\n"
                                    "  int m = mk()[2];\n" /* 3 */
                                    "  int w = 0;\n"
                                    "  while (w < (int)mk().length) { w = w + 1; }\n" /* 3 */
                                    "  int u = unbox() + mkb() - 41;\n"              /* 41 */
                                    "  int[] kept = mk();\n"
                                    "  ^int kb = mkb();\n"
                                    "  int moved = total(mk());\n" /* owned arg: 6 */
                                    "  return n + v + (int)t.length + e + m + w + u + kept[0] + kb + moved;\n"
                                    "}\n"),
                    3 + 41 + 2 + 1 + 3 + 3 + 41 + 1 + 41 + 6);
    STRATA_CHECK(g_cgAllocs > 0);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

/* An optional (a maybe-empty box cell) whose inner is an owning struct must
   free every level: the cell and R's owning fields. Drops recurse through
   the shared drop helper. (`^R?` is not a valid type; `R?` is the box.) */
STRATA_TEST(box_of_owning_box_drops_all_levels)
{
    STRATA_CHECK_EQ(RunEntryCounted("struct R { string name; int[] xs; };\n"
                                    "int entry() {\n"
                                    "  R?[] arr = { R { .name = \"abc\", .xs = {1, 2} }, R { .name = \"d\", .xs = {3} } };\n"
                                    "  R? o = arr[1];\n"
                                    "  return 1;\n"
                                    "}\n"),
                    1);
    STRATA_CHECK(g_cgAllocs > 0);
    STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
}

/* A `defer` that reads a variable the return expression only READS (not
   moves) runs after the return value is computed and sees the live value. */
STRATA_TEST(defer_reads_variable_after_return_reads_it)
{
    STRATA_CHECK_EQ(RunEntryCounted("struct B { int v; };\n"
                                    "void peek(ref ^B b, ref int out) { out = b.v * 2; }\n"
                                    "int f(ref int out) {\n"
                                    "  ^B b = B { .v = 21 };\n"
                                    "  defer peek(b, out);\n"
                                    "  return b.v;\n"
                                    "}\n"
                                    "int entry() { int o = 0; int r = f(o); return r + o; }\n"),
                    63);
    STRATA_CHECK_EQ(g_cgAllocs, 1);
    STRATA_CHECK_EQ(g_cgFrees, 1);
}

/* An OWNING piece read out of a temporary (`mkArr()[0]`, `mkS().name`) used
   to leave the whole temporary un-dropped (a leak): the piece might have been
   moved out. Now a move nulls the piece inside the temporary and the
   temporary is dropped at the end of the statement; a piece that is only
   read is dropped along with it. Every case must balance allocs and frees. */
#define TEMP_PART_PRELUDE \
    "struct R { string name; int[] xs; };\n" \
    "struct E { int v; };\n" \
    "struct H { string name; };\n" \
    "string[] mkArr() { string[] a = {\"ab\", \"cde\"}; return a; }\n" \
    "R mkR() { return R { .name = \"rname\", .xs = {1, 2} }; }\n" \
    "^R mkBR() { return R { .name = \"boxed\", .xs = {3} }; }\n" \
    "R[] mkRs() { R[] a = { R { .name = \"r0\", .xs = {1} }, R { .name = \"r11\", .xs = {2, 3} } }; return a; }\n" \
    "^E[] mkEs() { ^E[] a = { E { .v = 4 }, E { .v = 9 } }; return a; }\n" \
    "int[][] mkNested() { int[][] a = { {1, 2}, {3, 4, 5} }; return a; }\n" \
    "string first() { return mkArr()[0]; }\n" \
    "int slen(string s) { return (int)s.length; }\n" \
    "int takeE(^E e) { return e.v; }\n" \
    "int sum(int[] a) { int t = 0; for (uint i = 0; i < a.length; i++) { t = t + a[i]; } return t; }\n"

typedef struct
{
    const char* body;
    int expected;
} TempPartCase;

STRATA_TEST(owning_pieces_of_temporaries_are_dropped)
{
    static const TempPartCase cases[] = {
        /* moved into a local */
        {"int entry() { int[][] a = mkNested(); return a[1][2]; }", 5},
        {"int entry() { int[][] a = { {1, 2}, {3, 4, 5} }; return a[1][2]; }", 5},
        {"int entry() { int[] x = {7, 8}; int[][] a = { x, {1} }; return a[0][1]; }", 8},
        {"int entry() { string s = mkArr()[0]; return (int)s.length; }", 2},
        {"int entry() { string s = mkR().name; return (int)s.length; }", 5},
        {"int entry() { string s = mkBR().name; return (int)s.length; }", 5},
        {"int entry() { int[] x = mkR().xs; return x[1]; }", 2},
        {"int entry() { ^R r = mkRs()[1]; return (int)r.name.length + r.xs[1]; }", 6},
        {"int entry() { ^E e = mkEs()[0]; return e.v + mkEs()[1].v; }", 13},
        {"int entry() { int[] x = mkNested()[1]; return x[2]; }", 5},
        /* nested: a piece of a piece */
        {"int entry() { string s = mkRs()[1].name; return (int)s.length; }", 3},
        {"int entry() { int[] x = mkRs()[1].xs; return x[0] + x[1]; }", 5},
        /* only read: the whole temporary is dropped */
        {"int entry() { return (int)mkArr()[1].length; }", 3},
        {"int entry() { return (int)mkRs()[1].name.length + mkR().xs[0]; }", 4},
        {"int entry() { string s = mkBR().name; return (int)s.length + (int)mkBR().name.length; }", 10},
        /* passed to a parameter that takes ownership */
        {"int entry() { return slen(mkArr()[1]); }", 3},
        {"int entry() { return takeE(mkEs()[1]); }", 9},
        {"int entry() { return sum(mkNested()[1]); }", 12},
        {"int entry() { return sum(mkR().xs); }", 3},
        /* moved into a struct field */
        {"int entry() { ^H h = H { .name = mkArr()[1] }; return (int)h.name.length; }", 3},
        {"int entry() { ^H h = H(mkArr()[0]); return (int)h.name.length; }", 2},
        /* assigned, returned */
        {"int entry() { string s = \"x\"; s = mkArr()[1]; return (int)s.length; }", 3},
        {"int entry() { string s = first(); return (int)s.length; }", 2},
        /* repeated in a loop: one temporary per iteration */
        {"int entry() { int t = 0; for (int i = 0; i < 4; i++) { string s = mkArr()[1]; t = t + (int)s.length; } "
         "return t; }",
         12},
    };

    char src[4096];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        snprintf(src, sizeof(src), "%s%s\n", TEMP_PART_PRELUDE, cases[i].body);

        int got = RunEntryCounted(src);

        if (got != cases[i].expected || g_cgAllocs != g_cgFrees || g_cgAllocs == 0)
        {
            printf("  case %d: %s\n    result %d (want %d), allocs %d, frees %d\n", (int)i, cases[i].body, got,
                   cases[i].expected, g_cgAllocs, g_cgFrees);
        }

        STRATA_CHECK_EQ(got, cases[i].expected);
        STRATA_CHECK(g_cgAllocs > 0);
        STRATA_CHECK_EQ(g_cgFrees, g_cgAllocs);
    }
}
