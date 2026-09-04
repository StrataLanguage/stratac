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

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
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

STRATA_TEST(global_string_array_element_move_is_an_error)
{
    /* Reading an owning element out of a global array steals its pointer,
       leaving the slot dangling for the next reader (and double-freeing on
       teardown). Reject it with the same transitive global-move error as a
       whole-global move. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "string[] g = {\"hi\"};\n"
        "int entry() {\n"
        "  string s = g[0];\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    /* Element keys are spelled precisely ("g[0]"); the root check still
       reduces them to the global "g". */
    STRATA_CHECK(Contains(d, "g[0]' cannot be moved as it is not owned because it is global"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(global_owning_field_move_is_an_error)
{
    /* Transitive through a struct field: moving an owning field out of a
       global struct is rejected with the same error. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct Cell { string name; };\n"
        "^Cell g = Cell { .name = \"hi\" };\n"
        "int entry() {\n"
        "  string s = g.name;\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(Contains(d, "cannot be moved as it is not owned because it is global"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(global_string_array_element_copy_is_allowed)
{
    /* copy() is the escape hatch: it deep-copies the element so the global
       array keeps its value and the local independently owns its copy. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "extern ulong strlen(string s);\n"
        "string[] g = {\"hi\"};\n"
        "int entry() {\n"
        "  string s = copy(g[0]);\n"
        "  return (int)strlen(s);\n"            /* "hi" -> 2 */
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
        STRATA_CHECK_EQ(entry(), 2);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(string_equality_is_content_not_pointer)
{
    /* `==`/`!=` compares CONTENT (module-local strata_str_eq: length
       fast-out, pointer-identity fast-in, then bytes) — never the raw fat
       pointers. An owning local is a heap copy, so its pointer differs
       from the literal's constant fat even when the content matches. */
    const char* err = NULL;
    StrataJit* jit = CompileStr(
        "int entry() {\n"
        "  string s = \"hello\";\n"     /* owning local: heap copy, ptr != literal's fat */
        "  int r = 0;\n"
        "  if (s == \"hello\") { r += 1; }\n"     /* content equal, different pointers */
        "  if (s != \"world\") { r += 2; }\n"     /* same length, different content */
        "  if (s == \"world!\") { r += 4; } else { r += 4; }\n" /* length fast-out */
        "  if (s == \"\") { r += 8; } else { r += 8; }\n"       /* empty vs non-empty */
        "  if (\"\" == \"\") { r += 16; }\n"                    /* both empty {null, 0} */
        "  if (s != \"hello\") { r += 32; }\n"    /* != on equal content */
        "  string t = copy(s);\n"                /* deep copy: same content, fresh buffer */
        "  if (t == s) { r += 64; }\n"           /* byte compare across buffers */
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
        STRATA_CHECK_EQ(entry(), 95);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(string_vs_non_string_comparison_is_rejected)
{
    /* string ==/!= is content equality; comparing a string with a scalar
       is a compile error (never a fat-pointer compare). */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "int entry() {\n"
        "  string s = \"hi\";\n"
        "  if (s == 1) { return 1; }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "cannot compare 'string' with 'int'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
