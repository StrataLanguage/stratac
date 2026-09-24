#include "Util.h"
#include "Test.h"
#include "strata/strata.h"


static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "cfg", &err);
    
    if (err)
    {
        strataFree((char*)err);
    }

    return jit;
}

STRATA_TEST(jit_inout_param_writes_back)
{
    StrataJit* jit = CompileJit("void add_one(ref int x) { x = x + 1; }\n"
                                "int entry() { int n = 10; add_one(n); return n; }\n");
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 11);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_out_params_return_values)
{
    StrataJit* jit = CompileJit("void divmod(int a, int b, ref int q, ref int r) { q = a / b; r = a % b; }\n"
                                "int entry() {\n"
                                "  int q;\n"
                                "  int r;\n"
                                "  divmod(17, 5, q, r);\n"
                                "  return q * 100 + r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 302);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_large_loop_no_stack_overflow)
{
    StrataJit* jit = CompileJit("int entry() {\n"
                                "  int total = 0;\n"
                                "  for (int i = 0; i < 100000; i++) {\n"
                                "    int step = 1;\n"
                                "    total += step;\n"
                                "  }\n"
                                "  return total;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 100000);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_if_else_branch)
{
    StrataJit* jit = CompileJit("int sign(int n) { if (n < 0) { return -1; } else { return 1; } }\n"
                                "int absval(int n) { int r = n; if (r < 0) { r = 0 - r; } return r; }\n"
                                "int entry() { return sign(-5) * 10 + sign(3) + absval(-7); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), -2);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_while_loop)
{
    StrataJit* jit = CompileJit("int sumto(int n) {\n"
                                "  int s = 0;\n"
                                "  int i = 1;\n"
                                "  while (i <= n) { s = s + i; i = i + 1; }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sumto(100); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 5050);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_break_and_continue)
{
    StrataJit* jit = CompileJit("int sumskip(int n) {\n"
                                "  int s = 0;\n"
                                "  int i = 0;\n"
                                "  while (true) {\n"
                                "    i = i + 1;\n"
                                "    if (i > n) { break; }\n"
                                "    if (i == 5) { continue; }\n"
                                "    s = s + i;\n"
                                "  }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sumskip(10); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 50);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_recursion)
{
    StrataJit* jit = CompileJit("int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
                                "int entry() { return fib(10); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 55);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_for_loop)
{
    StrataJit* jit = CompileJit("int factorial(int n) {\n"
                                "  int r = 1;\n"
                                "  for (int i = 1; i <= n; i = i + 1) { r = r * i; }\n"
                                "  return r;\n"
                                "}\n"
                                "int sum_squares(int n) {\n"
                                "  int s = 0;\n"
                                "  for (int i = 1; i <= n; i = i + 1) { s = s + i * i; }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return factorial(5) + sum_squares(4); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 150);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_for_loop_with_continue)
{
    StrataJit* jit = CompileJit("int sum_skip_evens(int n) {\n"
                                "  int s = 0;\n"
                                "  for (int i = 1; i <= n; i = i + 1) {\n"
                                "    if (i == 2 || i == 4) { continue; }\n"
                                "    s = s + i;\n"
                                "  }\n"
                                "  return s;\n"
                                "}\n"
                                "int entry() { return sum_skip_evens(5); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 9);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(missing_return_on_branch_is_error)
{
    /* A non-void function whose false branch falls through has no return on
       that path -> compilation must fail. */
    StrataJit* jit = CompileJit("int f(int x) {\n"
                                "  if (x > 0) return 1;\n"
                                "}\n");
    STRATA_CHECK(jit == NULL);
    if (jit) strataJitDestroy(jit);
}

STRATA_TEST(missing_return_on_loop_zero_runs_is_error)
{
    /* A while may run zero times, so control can fall off the end. */
    StrataJit* jit = CompileJit("int g(int x) {\n"
                                "  while (x > 0) { return 1; }\n"
                                "}\n");
    STRATA_CHECK(jit == NULL);
    if (jit) strataJitDestroy(jit);
}

STRATA_TEST(missing_return_break_then_fallthrough_is_error)
{
    /* while(true){break;} exits the loop and falls off the end. */
    StrataJit* jit = CompileJit("int k() {\n"
                                "  while (true) { break; }\n"
                                "}\n");
    STRATA_CHECK(jit == NULL);
    if (jit) strataJitDestroy(jit);
}

STRATA_TEST(missing_return_if_else_both_return_ok)
{
    /* Both branches return, so no missing return. */
    StrataJit* jit = CompileJit("int f(int x) {\n"
                                "  if (x > 0) return 1; else return 2;\n"
                                "}\n"
                                "int entry() { return f(1); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit) strataJitDestroy(jit);
}

STRATA_TEST(missing_return_while_true_infinite_ok)
{
    /* while(true){return;} never falls through (infinite loop). */
    StrataJit* jit = CompileJit("int h() {\n"
                                "  while (true) { return 1; }\n"
                                "}\n"
                                "int entry() { return h(); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit) strataJitDestroy(jit);
}

/* ---- defer is checked where it runs ----------------------------------------
   A deferred statement runs at every exit of its scope (end, return, break,
   continue), after everything before that exit, so its moves and reads are
   checked against the state at each of those exits, LIFO. */

#define DEFER_FLOW_PRELUDE                                \
    "struct E { int v; };\n"                              \
    "^E mk(int n) { return E { .v = n }; }\n"             \
    "int take(^E e) { return e.v; }\n"                    \
    "int note(int x) { return x; }\n"

/* Error count of resolving `src`, or -1 when it has none of `needle`. */
static int DeferErrors(const char* src, const char* needle)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    ParseAndResolve(src, &diag, &arena);

    int count = (int)DiagErrorCount(&diag);
    bool hit = false;

    for (size_t i = 0; i < diag.m_count; i++)
    {
        hit = hit || strstr(diag.m_diagnostics[i].message, needle) != NULL;
    }

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return count > 0 && !hit ? -1 : count;
}

STRATA_TEST(defer_reading_box_moved_before_exit_is_an_error)
{
    /* The deferred read runs after `take(b)` freed the box. */
    STRATA_CHECK(DeferErrors(DEFER_FLOW_PRELUDE
                             "void entry() { ^E b = mk(1); defer note(b.v); take(b); }\n",
                             "used after move")
                 > 0);

    /* Only the early-return exit moved it. */
    STRATA_CHECK(DeferErrors(DEFER_FLOW_PRELUDE
                             "int entry(bool c) {\n"
                             "  ^E b = mk(1); defer note(b.v);\n"
                             "  if (c) { take(b); return 1; }\n"
                             "  return 0;\n"
                             "}\n",
                             "used after move")
                 > 0);

    /* The `break` exit of a loop-body scope. */
    STRATA_CHECK(DeferErrors(DEFER_FLOW_PRELUDE
                             "int entry() {\n"
                             "  int i = 0;\n"
                             "  while (i < 3) { i++; ^E b = mk(1); defer note(b.v); take(b); if (i == 1) { break; } }\n"
                             "  return 0;\n"
                             "}\n",
                             "used after move")
                 > 0);

    /* The `continue` exit runs the defer before `b` is re-lived. */
    STRATA_CHECK(DeferErrors(DEFER_FLOW_PRELUDE
                             "int entry() {\n"
                             "  ^E b = mk(1); int i = 0;\n"
                             "  while (i < 3) { i++; defer take(b); if (i == 1) { continue; } b = mk(2); }\n"
                             "  return 0;\n"
                             "}\n",
                             "used after move")
                 > 0);
}

STRATA_TEST(defer_same_error_at_several_exits_is_reported_once)
{
    STRATA_CHECK_EQ(DeferErrors(DEFER_FLOW_PRELUDE
                                "int entry(bool c) {\n"
                                "  ^E b = mk(1); defer note(b.v);\n"
                                "  if (c) { take(b); return 1; }\n"
                                "  if (!c) { take(b); return 2; }\n"
                                "  take(b);\n"
                                "  return 0;\n"
                                "}\n",
                                "used after move"),
                    1);
}

STRATA_TEST(defer_cannot_see_names_declared_after_it)
{
    STRATA_CHECK(DeferErrors(DEFER_FLOW_PRELUDE "int entry() { defer note(x); int x = 1; return x; }\n",
                             "unknown variable 'x'")
                 > 0);
}

STRATA_TEST(defer_move_after_return_value_is_legal)
{
    /* `return b.v` reads the box first; the deferred `take(b)` then consumes
       it. Nothing is used after the move. */
    STRATA_CHECK_EQ(DeferErrors(DEFER_FLOW_PRELUDE
                                "int entry() { ^E b = mk(7); defer note(1); defer take(b); return b.v; }\n"
                                "int loop() {\n"
                                "  int i = 0;\n"
                                "  while (i < 3) {\n"
                                "    i++; ^E b = mk(i); defer take(b);\n"
                                "    if (i == 1) { continue; }\n"
                                "    if (i == 2) { break; }\n"
                                "    note(b.v);\n"
                                "  }\n"
                                "  return i;\n"
                                "}\n"
                                "void reads() { ^E b = mk(1); defer note(b.v); note(b.v); }\n",
                                "used after move"),
                    0);

    StrataJit* jit = CompileJit(DEFER_FLOW_PRELUDE
                                "int entry() { ^E b = mk(7); defer take(b); return b.v; }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(NULL), 7);
        }
        strataJitDestroy(jit);
    }
}
