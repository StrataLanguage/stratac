#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

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
