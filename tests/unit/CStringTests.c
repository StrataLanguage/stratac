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
