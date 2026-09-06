#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <string.h>
#include <stdio.h>

/* ================= Lexer ================= */

STRATA_TEST(private_at_lexes_as_at_token)
{
    TokenList t = LexAll("@private");
    TokKind* k = Kinds(t);
    STRATA_CHECK(k[0] == TokAt);
    STRATA_CHECK(k[1] == TokIdent);
    STRATA_CHECK(StrEqC(TextOf("@private", t.items[1]), "private"));
    STRATA_CHECK(k[t.count - 1] == TokEof);
    free(k);
    free(t.items);
}

/* ================= Parser ================= */

STRATA_TEST(private_function_prototype_parsed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@private\nvoid dosomething(int x, int y);", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count == 1);

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(fn->isPrivate);
    STRATA_CHECK(strcmp(fn->name, "dosomething") == 0);
    STRATA_CHECK_EQ((long)fn->params.count, 2);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(private_function_with_body_parsed)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@private\nint secret() { return 7; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(fn->isPrivate);
    STRATA_CHECK(fn->body != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(plain_function_is_not_private)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return 1; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(!fn->isPrivate);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(private_stacked_duplicate_ok)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@private\n@private\nint f() { return 1; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(fn->isPrivate);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(unknown_attribute_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@nope\nint f() { return 1; }", &diag, &arena);
    (void)mod;
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(private_on_global_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@private\nint g;", &diag, &arena);
    (void)mod;
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(private_on_struct_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@private\nstruct S { int x; };", &diag, &arena);
    (void)mod;
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(private_on_impl_method_rejected)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("handle H;\nimpl H {\n@private\nextern void M(H self);\n}", &diag, &arena);
    (void)mod;
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ================= Cross-file visibility (virtual modules) ================= */

static int ResolverPrivateLib(void* userData, const char* importerName, const char* importPath,
                              StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;

    if (strcmp(importPath, "lib") == 0)
    {
        out->text = "@private\n"
                    "int secret() { return 7; }\n"
                    "int helper() { return secret(); }\n";
        out->length = strlen(out->text);
        out->name = "lib";
        return 1;
    }

    return 0;
}

STRATA_TEST(private_same_file_call_runs)
{
    /* `helper` (public) calls `secret` (@private) from the same file: legal. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverPrivateLib, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import lib;\n"
        "int entry() { return helper(); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(private_cross_file_call_hidden)
{
    /* `secret` is @private in "lib": a call from "main" must behave as if it
       does not exist. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverPrivateLib, NULL);

    StrataResult r = strataCompileString(c,
        "import lib;\n"
        "int entry() { return secret(); }\n",
        "main", STRATA_EMIT_LLVM_IR, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "unknown function 'secret'") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(private_single_file_program_runs)
{
    /* In a single file, @private is a no-op: same-file calls work. */
    StrataCompiler* c = strataCompilerCreate();

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "@private\n"
        "int secret() { return 9; }\n"
        "int entry() { return secret(); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 9);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* One overload private, the other public: each keeps its own visibility. */
static int ResolverPrivateOverloads(void* userData, const char* importerName, const char* importPath,
                                    StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;

    if (strcmp(importPath, "lib") == 0)
    {
        out->text = "@private\n"
                    "int pick(int x) { return x + 1; }\n"
                    "int pick(int x, int y) { return x + y; }\n";
        out->length = strlen(out->text);
        out->name = "lib";
        return 1;
    }

    return 0;
}

STRATA_TEST(private_overload_hidden_public_overload_visible)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverPrivateOverloads, NULL);

    /* Public 2-arg overload callable cross-file. */
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import lib;\n"
        "int entry() { return pick(3, 4); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(entry != NULL);
        if (entry)
        {
            STRATA_CHECK_EQ(entry(), 7);
        }
        strataJitDestroy(jit);
    }
    else
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
    }
    strataCompilerDestroy(c);

    /* Private 1-arg overload invisible cross-file. */
    c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverPrivateOverloads, NULL);

    StrataResult r = strataCompileString(c,
        "import lib;\n"
        "int entry() { return pick(3); }\n",
        "main", STRATA_EMIT_LLVM_IR, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "pick") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}
