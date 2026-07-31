#include "Util.h"
#include "strata/Test.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

static void SamplePath(const char* rel, char* out, size_t outCap)
{
    snprintf(out, outCap, "%s/imports/%s", STRATA_SAMPLE_DIR, rel);
}

STRATA_TEST(import_function_jit_runs)
{
    char path[512];
    SamplePath("main.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 25);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(import_cycle_jit_runs)
{
    char path[512];
    SamplePath("cycle_a.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    int (*a_value)(void) = (int (*)(void))strataJitGetFunction(jit, "a_value");
    STRATA_CHECK(a_value != NULL);
    if (a_value)
    {
        STRATA_CHECK_EQ(a_value(), 42);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(import_compiles_to_merged_ir)
{
    char path[512];
    SamplePath("main.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR);

    if (!r.ok)
    {
        printf("  compile failed: %s\n", r.diagnostics);
    }

    STRATA_CHECK(r.ok);
    STRATA_CHECK(strstr(r.output, "makeDamage") != NULL);
    STRATA_CHECK(strstr(r.output, "entry") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(import_missing_file_is_reported)
{
    char path[512];
    SamplePath("missing.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "cannot open module") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(import_from_string_is_rejected)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c,
        "import weapons/pistol;\n"
        "int entry() { return 1; }\n",
        "inline", STRATA_EMIT_LLVM_IR);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(strstr(r.diagnostics, "not supported") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(import_error_names_imported_file)
{
    char path[512];
    SamplePath("uses_broken.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(strstr(r.diagnostics, "broken_mod.strata") != NULL);
    STRATA_CHECK(strstr(r.diagnostics, "unknown variable") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}
