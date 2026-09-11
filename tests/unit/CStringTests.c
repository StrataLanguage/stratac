/* CStringTests.c — `cstring`: a constant string stored in read-only data and
 * passed to C as a `const char*`.
 *
 *   cstring s = "hello";   // contextual literal -> .rodata pointer
 *   puts(s);               // extern param crosses as const char*
 *   s.length / s[i] / s == "..."   // content semantics
 *   string t = s;          // one-way copy: cstring -> string
 */

#include "strata/strata.h"

#include "Test.h"
#include "Util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static StrataJit* CompileStr(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "cstr", err);
    strataCompilerDestroy(c);
    return jit;
}

/* Requires a diagnostic from a source snippet. */
static bool Rejects(const char* src)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Arena arena;
    arena_init(&arena, 0);

    ParseAndResolve(src, &diag, &arena);

    bool bad = DiagHasErrors(&diag);
    arena_free(&arena);
    DiagnosticEngineFree(&diag);
    return bad;
}

/* ---- Host ABI shims ---- */

static int HostCSLen(const char* s)
{
    return (int)strlen(s);
}

static int HostCSEq(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

static const char* g_hostCS = "host-returned";

static const char* HostCSGet(void)
{
    return g_hostCS;
}

STRATA_TEST(cstring_var_decl_length_and_index)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int entry()\n"
                                "{\n"
                                "  cstring s = \"hello\";\n"
                                "  byte c = s[1];\n"
                                "  return (int)s.length + (int)c;\n"
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
        STRATA_CHECK_EQ(entry(), 5 + 'e');
    }

    strataJitDestroy(jit);
}

STRATA_TEST(cstring_equality_is_content)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int entry()\n"
                                "{\n"
                                "  cstring a = \"hello\";\n"
                                "  cstring b = \"hel\";\n"
                                "  cstring c = \"hello\";\n"
                                "  int r = 0;\n"
                                "  if (a == c) { r = r + 1; }\n"
                                "  if (a != b) { r = r + 2; }\n"
                                "  if (a == \"hello\") { r = r + 4; }\n"
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

STRATA_TEST(cstring_copies_into_string)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int len_of(string s) { return (int)s.length; }\n"
                                "int entry()\n"
                                "{\n"
                                "  cstring c = \"copied\";\n"
                                "  string s = c;\n"
                                "  string t = (string)c;\n"
                                "  return len_of(s) + (int)t.length;\n"
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

STRATA_TEST(cstring_return_from_function_and_global)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("cstring pick() { return \"picked\"; }\n"
                                "cstring g = \"global\";\n"
                                "int entry()\n"
                                "{\n"
                                "  cstring p = pick();\n"
                                "  return (int)p.length + (int)g.length;\n"
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
        STRATA_CHECK_EQ(entry(), 6 + 6);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(cstring_extern_param_receives_const_char_ptr)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
                                            "extern int cs_len(cstring s);\n"
                                            "extern int cs_eq(cstring a, cstring b);\n"
                                            "int entry()\n"
                                            "{\n"
                                            "  cstring a = \"hello\";\n"
                                            "  int r = cs_len(a) + cs_len(\"world\");\n"
                                            "  if (cs_eq(a, \"hello\")) { r = r + 1; }\n"
                                            "  return r;\n"
                                            "}\n",
                                            "cshost", &err);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    strataJitAddSymbol(jit, "cs_len", (void*)&HostCSLen);
    strataJitAddSymbol(jit, "cs_eq", (void*)&HostCSEq);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 5 + 5 + 1);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(cstring_extern_return_is_char_ptr)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
                                            "extern cstring cs_get();\n"
                                            "int entry()\n"
                                            "{\n"
                                            "  cstring s = cs_get();\n"
                                            "  string owned = cs_get();\n"
                                            "  return (int)s.length + (int)owned.length;\n"
                                            "}\n",
                                            "csret", &err);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    strataJitAddSymbol(jit, "cs_get", (void*)&HostCSGet);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 13 + 13);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(cstring_array_literal_and_push)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int entry()\n"
                                "{\n"
                                "  cstring[] a = {\"one\", \"two\", \"three\"};\n"
                                "  int r = (int)a.length + (int)a[1].length;\n"
                                "  array_push(a, \"four\");\n"
                                "  return r + (int)a.length + (int)a[3].length;\n"
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
        /* 3 + 3 (two) + 4 (new length) + 4 (four) */
        STRATA_CHECK_EQ(entry(), 14);
    }

    strataJitDestroy(jit);
}

/* ---- Rejected forms ---- */

STRATA_TEST(cstring_rejects_no_box_or_optional)
{
    STRATA_CHECK(Rejects("int f() { cstring? s; return 0; }\n"));
    STRATA_CHECK(Rejects("int f() { ^cstring s; return 0; }\n"));
}

STRATA_TEST(cstring_rejects_string_to_cstring)
{
    /* string -> cstring is never implicit (the copy is one-way). */
    STRATA_CHECK(Rejects("int f() {\n"
                         "  string s = \"hi\";\n"
                         "  cstring c = s;\n"
                         "  return 0;\n"
                         "}\n"));
}

STRATA_TEST(cstring_rejects_element_write_and_cap)
{
    STRATA_CHECK(Rejects("int f() {\n"
                         "  cstring c = \"hi\";\n"
                         "  c[0] = 65;\n"
                         "  return 0;\n"
                         "}\n"));
    STRATA_CHECK(Rejects("int f() {\n"
                         "  cstring c = \"hi\";\n"
                         "  return (int)c.cap;\n"
                         "}\n"));
}

/* ---- Regression: a cstring crossing into OWNING string contexts is a
   heap copy at the use site, for EVERY expression form (not just a bare
   identifier or literal). Before the fix, any other form emitted a
   mismatched ABI (raw char* where the callee reads a fat) and crashed. ---- */

/* Host shim: a fresh buffer with the SAME content — different address than
   any literal, so content-vs-pointer equality is distinguishable. */
static char* HostDupStr(const char* s)
{
    char* p = (char*)malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

STRATA_TEST(string_assign_cstring_value)
{
    /* `s = cs` (rebind of an owning local) must heap-copy the cstring. */
    const char* err = NULL;
    StrataJit* jit = CompileStr("int entry()\n"
                                "{\n"
                                "  cstring cs = \"hello\";\n"
                                "  string s = \"x\";\n"
                                "  s = cs;\n"
                                "  return (int)s.length;\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 5);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(string_arg_move_semantics_preserved)
{
    /* A plain string arg keeps its move semantics: the source is emptied
       and rebinding afterwards neither crashes nor double-frees. */
    const char* err = NULL;
    StrataJit* jit = CompileStr("int len_of(string s) { return (int)s.length; }\n"
                                "int entry()\n"
                                "{\n"
                                "  string s = \"moved\";\n"
                                "  int a = len_of(s);\n"
                                "  s = \"again\";\n"
                                "  return a + (int)s.length;\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 10);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(cstring_element_arg_copies_into_string_param)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int len_of(string s) { return (int)s.length; }\n"
                                "int entry()\n"
                                "{\n"
                                "  cstring[] arr = {\"hello\", \"hi\"};\n"
                                "  return len_of(arr[0]);\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 5);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(cstring_member_of_call_result_arg)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("struct Pair { cstring name; };\n"
                                "Pair GetPair() { return Pair { .name = \"member\" }; }\n"
                                "int len_of(string s) { return (int)s.length; }\n"
                                "int entry()\n"
                                "{\n"
                                "  return len_of(GetPair().name);\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 6);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(cstring_push_and_element_write_into_string_array)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("int entry()\n"
                                "{\n"
                                "  string[] arr = {};\n"
                                "  cstring c = \"pushed\";\n"
                                "  array_push(arr, c);\n"
                                "  string[] arr2 = {\"a\"};\n"
                                "  cstring c2 = \"elem\";\n"
                                "  arr2[0] = c2;\n"
                                "  return (int)arr[0].length + (int)arr2[0].length;\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 10);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(cstring_global_initialized_from_global)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr("cstring g = \"global\";\n"
                                "cstring g2 = g;\n"
                                "int entry()\n"
                                "{\n"
                                "  return (int)g2.length + (int)g.length;\n"
                                "}\n",
                                &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);

        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 12);
        }

        strataJitDestroy(jit);
    }

    strataFree((char*)err);
}

STRATA_TEST(cstring_struct_field_equality_is_content)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
                                            "extern cstring dup_str(cstring s);\n"
                                            "struct Wrapper { cstring s; int n; };\n"
                                            "int entry()\n"
                                            "{\n"
                                            "  Wrapper a = Wrapper { .s = \"dup\", .n = 1 };\n"
                                            "  Wrapper b;\n"
                                            "  b.s = dup_str(\"dup\");\n"
                                            "  b.n = 1;\n"
                                            "  if (a == b) { return 7; }\n"
                                            "  return 3;\n"
                                            "}\n",
                                            "cseq",
                                            &err);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    strataJitAddSymbol(jit, "dup_str", (void*)&HostDupStr);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(cstring_array_equality_is_content)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
                                            "extern cstring dup_str(cstring s);\n"
                                            "int entry()\n"
                                            "{\n"
                                            "  cstring[] a = {\"one\", \"two\"};\n"
                                            "  cstring[] b = {dup_str(\"one\"), dup_str(\"two\")};\n"
                                            "  if (a == b) { return 7; }\n"
                                            "  return 3;\n"
                                            "}\n",
                                            "csaeq",
                                            &err);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    strataJitAddSymbol(jit, "dup_str", (void*)&HostDupStr);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
