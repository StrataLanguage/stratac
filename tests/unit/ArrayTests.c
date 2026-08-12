#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

#if STRATA_TEST_HAS_LLVM
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include <stdint.h>
#endif

static StrataJit* CompileArr(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "arr", err);
    strataCompilerDestroy(c);
    return jit;
}

STRATA_TEST(array_literal_and_index)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3, 4};\n"
        "  return a[0] + a[1] + a[2] + a[3];\n"   /* 1+2+3+4 = 10 */
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

STRATA_TEST(array_of_strings_iterates_and_drops)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  string[] names = {\"alpha\", \"beta\", \"gamma\"};\n"
        "  int n = 0;\n"
        "  for (ulong i = 0; i < names.length; i = i + 1) {\n"
        "    n = n + 1;\n"
        "  }\n"
        "  return n;\n"                          /* 3; freeing 3 strings must not crash */
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

STRATA_TEST(array_ref_param_borrows)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int first(ref int[] a) {\n"
        "  return a[0];\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = {42, 7};\n"
        "  int f = first(a);\n"                  /* borrow: a still usable */
        "  return f + (int)a.length;\n"          /* 42 + 2 = 44 */
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
        STRATA_CHECK_EQ(entry(), 44);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_rebind_replaces_contents)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  a = {10, 20};\n"                      /* rebind: free old, take new */
        "  return a[0] + a[1] + (int)a.length;\n" /* 10 + 20 + 2 = 32 */
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

STRATA_TEST(array_move_value_is_owned_by_dest)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {5, 6, 7};\n"
        "  int[] b = a;\n"                       /* move: b owns the buffer now */
        "  return b[2];\n"                       /* 7 (reading a after this is a sema error) */
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

STRATA_TEST(array_in_loop_does_not_crash)
{
    /* A fresh array per iteration is freed each time (block scope). */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int sum = 0;\n"
        "  for (int i = 0; i < 50; i = i + 1) {\n"
        "    int[] a = {i, i, i};\n"
        "    sum = sum + a[0] + a[1] + a[2];\n"
        "  }\n"
        "  return sum;\n"                        /* 3 * (0+1+..+49) = 3*1225 = 3675 */
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
        STRATA_CHECK_EQ(entry(), 3675);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_returned_from_function)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] three() {\n"
        "  return {10, 20, 30};\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = three();\n"
        "  return a[0] + a[1] + a[2];\n"         /* 60 */
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
        STRATA_CHECK_EQ(entry(), 60);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_of_structs_sums_field)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct Pt { int x; int y; };\n"
        "int entry() {\n"
        "  Pt[] pts = { Pt{ .x = 1, .y = 2 }, Pt{ .x = 3, .y = 4 } };\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < pts.length; i = i + 1) {\n"
        "    total = total + pts[i].x + pts[i].y;\n"
        "  }\n"
        "  return total;\n"                       /* (1+2) + (3+4) = 10 */
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

STRATA_TEST(array_use_after_move_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "void take(int[] a) {}\n"
        "int entry() {\n"
        "  int[] a = {1,2,3};\n"
        "  take(a);\n"
        "  return (int)a.length;\n"              /* used after move into take() */
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_rejects_non_array_argument)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int x = 5;\n"
        "  array_push(x, 1);\n"              /* x is not an array */
        "  return x;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_rejects_wrong_arg_count)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a);\n"                 /* missing value */
        "  return (int)a.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_push_owning_value_from_index_is_error)
{
    /* Pushing an owning value read out of an array element would duplicate
       ownership (double-free); it must be rejected. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  string[] s = {\"a\", \"b\"};\n"
        "  array_push(s, s[0]);\n"           /* borrowed owning value */
        "  return (int)s.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_of_owning_struct_must_be_boxed)
{
    /* A struct holding an owning field is itself owning; holding it by value
       as an array element would leak its fields, so it must be boxed. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "struct S { string s; };\n"
        "int entry() {\n"
        "  S[] arr = { S{.s = \"a\"} };\n"   /* illegal: use box<S>[] */
        "  return (int)arr.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_uninitialized_decl_allowed_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a;\n"
        "  return (int)a.length;\n"              /* 0 */
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

STRATA_TEST(global_array_uninitialized_is_empty)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g;\n"
        "int entry() {\n"
        "  return (int)g.length;\n"            /* 0 */
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

STRATA_TEST(global_array_initializer_can_be_read)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3};\n"
        "int entry() {\n"
        "  return g[0] + g[1] + g[2] + (int)g.length;\n"   /* 1+2+3 + 3 = 9 */
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

STRATA_TEST(global_array_mutations_persist_across_calls)
{
    /* A global outlives each function call: pushes from one call are visible
       to the next. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2};\n"
        "int add_one() {\n"
        "  array_push(g, 9);\n"
        "  return (int)g.length;\n"
        "}\n"
        "int entry() {\n"
        "  int a = add_one();\n"               /* {1,2,9} -> 3 */
        "  int b = add_one();\n"               /* {1,2,9,9} -> 4 */
        "  return a + b + g[3];\n"             /* 3 + 4 + 9 = 16 */
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
        STRATA_CHECK_EQ(entry(), 16);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_is_mutable_from_functions)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3};\n"
        "void setit() {\n"
        "  g[0] = 99;\n"
        "}\n"
        "int entry() {\n"
        "  setit();\n"
        "  return g[0] + g[1] + g[2];\n"        /* 99 + 2 + 3 = 104 */
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
        STRATA_CHECK_EQ(entry(), 104);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(global_array_resize)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = {1, 2, 3, 4, 5};\n"
        "int entry() {\n"
        "  array_resize(g, 3);\n"
        "  return g[0] + g[1] + g[2] + (int)g.length;\n"   /* 1+2+3 + 3 = 9 */
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

STRATA_TEST(global_array_of_strings_drops_cleanly)
{
    /* The module teardown frees every string in the global array; this must
       not crash or double-free. */
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "string[] g = {\"alpha\", \"beta\", \"gamma\"};\n"
        "int entry() {\n"
        "  return (int)g.length;\n"            /* 3 */
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

STRATA_TEST(global_array_wrong_initializer_type_is_error)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int[] g = 5;\n"                       /* not an array initializer */
        "int entry() {\n"
        "  return (int)g.length;\n"
        "}\n",
        &err);

    STRATA_CHECK(jit == NULL);
    STRATA_CHECK(err != NULL);
    strataFree((char*)err);
}

STRATA_TEST(array_length_is_count)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "ulong entry() {\n"
        "  int[] a = {10, 20, 30, 40, 50};\n"
        "  return a.length;\n"                    /* 5 */
        "}\n",
        &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    unsigned long long (*entry)(void) =
        (unsigned long long (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 5ULL);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_index_is_mutable)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  a[1] = 20;\n"
        "  return a[1];\n"                        /* 20 */
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
        STRATA_CHECK_EQ(entry(), 20);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(array_iterate_and_sum)
{
    const char* err = NULL;
    StrataJit* jit = CompileArr(
        "int entry() {\n"
        "  int[] a = {1, 2, 3, 4};\n"
        "  int sum = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) {\n"
        "    sum = sum + a[i];\n"
        "  }\n"
        "  return sum;\n"                         /* 10 */
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

/* ---- Allocation/free balance for arrays returned from functions ---- */

static int g_arrAllocs = 0;
static int g_arrFrees = 0;

static void* ArrCountAlloc(unsigned long long n)
{
    g_arrAllocs++;
    return malloc((size_t)n);
}

/* Only non-null frees count as real frees: the LLVM backend emits an
   unconditional strata_free(NULL) after a moved-out source is nulled,
   which is harmless but would otherwise skew the balance. */
static void ArrCountFree(void* p)
{
    if (p)
    {
        g_arrFrees++;
    }
    free(p);
}

STRATA_TEST(array_return_cleans_up_memory)
{
    /* One function returns an array to another that hands it back to the
       caller. Every buffer must be freed exactly once: no leak, no
       double-free. Returning an array moves it out of the callee (the
       source slot is zeroed), and the caller owns the buffer until scope
       exit. The C backend must zero the moved source as a whole
       (strata__arr){0} struct, not a bare pointer zero. */
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, (void*)&ArrCountAlloc, (void*)&ArrCountFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "int[] build(int n) {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a, n);\n"              /* reallocates: alloc + free */
        "  return a;\n"                      /* moves a out */
        "}\n"
        "int[] relay(int n) {\n"
        "  int[] a = build(n);\n"            /* owns build's array */
        "  return a;\n"                      /* moves it out again */
        "}\n"
        "int sum(ref int[] a) {\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) { total = total + a[i]; }\n"
        "  return total;\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = relay(4);\n"            /* {1, 2, 3, 4} */
        "  int s = sum(a);\n"                /* 10; a borrowed, still live */
        "  return s + a[0] + a[3];\n"        /* 10 + 1 + 4 = 15 */
        "}\n",
        "arrclean", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    g_arrAllocs = 0;
    g_arrFrees = 0;

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 15);
        STRATA_CHECK(g_arrAllocs > 0);
        STRATA_CHECK_EQ(g_arrAllocs, g_arrFrees);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(array_return_cleans_up_memory_llvm)
{
    /* The same chained array return through the LLVM backend: the caller
       owns the buffer until scope exit, and every buffer is freed exactly
       once (null frees are not counted). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int[] build(int n) {\n"
        "  int[] a = {1, 2, 3};\n"
        "  array_push(a, n);\n"
        "  return a;\n"
        "}\n"
        "int[] relay(int n) {\n"
        "  int[] a = build(n);\n"
        "  return a;\n"
        "}\n"
        "int sum(ref int[] a) {\n"
        "  int total = 0;\n"
        "  for (ulong i = 0; i < a.length; i = i + 1) { total = total + a[i]; }\n"
        "  return total;\n"
        "}\n"
        "int entry() {\n"
        "  int[] a = relay(4);\n"
        "  int s = sum(a);\n"
        "  return s + a[0] + a[3];\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, true);
    LLVMJit jit;
    LLVMJitInit(&jit);
    LLVMJitSetAllocFree(&jit, (void*)&ArrCountAlloc, (void*)&ArrCountFree);
    char* err = NULL;
    bool ok = LLVMJitLoad(&jit, &bm, &err);
    if (!ok)
    {
        printf("  LLVM JIT failed: %s\n", err ? err : "(none)");
    }
    STRATA_CHECK(ok);

    g_arrAllocs = 0;
    g_arrFrees = 0;

    if (ok)
    {
        int (*entry)(void) = (int (*)(void))(uintptr_t)LLVMJitGetAddress(&jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 15);
            STRATA_CHECK(g_arrAllocs > 0);
            STRATA_CHECK_EQ(g_arrAllocs, g_arrFrees);
        }
    }

    free(err);
    LLVMJitDestroy(&jit);
    BuiltModuleDispose(&bm);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
#endif
