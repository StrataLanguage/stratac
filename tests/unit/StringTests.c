#include "Util.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

static StrataJit* CompileStr(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "str", err);
    strataCompilerDestroy(c);
    return jit;
}

STRATA_TEST(string_var_decl_and_use)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr(
        "int entry() {\n"
        "  string s = \"hello\";\n"
        "  return 42;\n"
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
        int r = entry();
        STRATA_CHECK_EQ(r, 42);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(string_lex_and_parse_basics)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Arena arena;
    arena_init(&arena, 0);

    Module* mod = ParseAndResolve(
        "string greeting(string name) {\n"
        "  string s = \"Hello, \";\n"
        "  return s;\n"
        "}\n",
        &diag, &arena);

    STRATA_CHECK(mod != NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK(mod->functions.count == 1);
    FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(strcmp(f->name, "greeting") == 0);
    STRATA_CHECK(strcmp(f->returnType.name, "string") == 0);

    arena_free(&arena);
    DiagnosticEngineFree(&diag);
}

STRATA_TEST(string_lexer_tokens)
{
    TokenList tl = LexAll("\"hello\\nworld\"");
    STRATA_CHECK(tl.count == 2);
    TokKind* k = Kinds(tl);
    STRATA_CHECK(k[0] == TokStrLit);
    STRATA_CHECK(k[1] == TokEof);
    free(k);
    free(tl.items);
}

STRATA_TEST(string_type_is_owning)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr(
        "string make_string() {\n"
        "  string s = \"created\";\n"
        "  return s;\n"
        "}\n"
        "int entry() {\n"
        "  string s = make_string();\n"
        "  return 7;\n"
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
        int r = entry();
        STRATA_CHECK_EQ(r, 7);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(string_reassign_literal)
{
    const char* err = NULL;
    StrataJit* jit = CompileStr(
        "int entry() {\n"
        "  string s = \"first\";\n"
        "  s = \"second\";\n"
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
        int r = entry();
        STRATA_CHECK_EQ(r, 1);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(string_type_check_rejects_int_init)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Arena arena;
    arena_init(&arena, 0);

    ParseAndResolve(
        "int f() {\n"
        "  string s = 42;\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    arena_free(&arena);
    DiagnosticEngineFree(&diag);
}

STRATA_TEST(global_string_array_element_assigned_to_local)
{
    /* A global string[] holding a single empty string; reading that element
       into a local owning string must copy the empty string correctly (and
       not double-free on teardown). Verified through a host strlen. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "extern ulong strlen(string s);\n"
        "string[] g = {\"\"};\n"
        "int entry() {\n"
        "  string s = g[0];\n"
        "  return (int)strlen(s);\n"            /* empty -> 0 */
        "}\n",
        "str", &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "strlen", (void*)&strlen), 1);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 0);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
