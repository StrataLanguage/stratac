#include "Test.h"
#include "Util.h"
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
        STRATA_CHECK_EQ(entry(), 5);
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

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(import_compiles_to_merged_ir)
{
    char path[512];
    SamplePath("main.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR, 0);

    if (!r.ok)
    {
        printf("  compile failed: %s\n", r.diagnostics);
    }

    STRATA_CHECK(r.ok);
    STRATA_CHECK(strstr(r.output, "MakePistol") != NULL);
    STRATA_CHECK(strstr(r.output, "entry") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}
#endif

STRATA_TEST(import_missing_file_is_reported)
{
    char path[512];
    SamplePath("missing.strata", path, sizeof(path));

    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_C, 0);

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
                                         "inline", STRATA_EMIT_C, 0);

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
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_C, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(strstr(r.diagnostics, "broken_mod.strata") != NULL);
    STRATA_CHECK(strstr(r.diagnostics, "unknown variable") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

/* ================= Custom import resolver ================= */

/* A resolver that serves a single virtual module named "lib". */
static int ResolverProvidesLib(void* userData, const char* importerName, const char* importPath,
                               StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;

    if (strcmp(importPath, "lib") == 0)
    {
        out->text = "int lib_answer() { return 42; }";
        out->length = strlen(out->text);
        out->name = "lib";
        return 1;
    }

    return 0;
}

STRATA_TEST(resolver_provides_virtual_module)
{
    /* A string source with an import, resolved entirely by the host resolver
       (no disk access). */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverProvidesLib, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import lib;\n"
        "int entry() { return lib_answer(); }\n",
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
        STRATA_CHECK_EQ(entry(), 42);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* A resolver that serves two virtual modules that import each other. */
static int ResolverCycle(void* userData, const char* importerName, const char* importPath, StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;

    if (strcmp(importPath, "a") == 0)
    {
        out->text = "import b;\n"
                    "int a_val() { return b_val() + 1; }\n";
        out->length = strlen(out->text);
        out->name = "a";
        return 1;
    }

    if (strcmp(importPath, "b") == 0)
    {
        out->text = "import a;\n"
                    "int b_val() { return 41; }\n";
        out->length = strlen(out->text);
        out->name = "b";
        return 1;
    }

    return 0;
}

STRATA_TEST(resolver_handles_import_cycle)
{
    /* a imports b, b imports a: the resolver's canonical names drive cycle
       detection, so the graph terminates and merges correctly. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverCycle, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import a;\n"
        "int entry() { return a_val(); }\n",
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
        STRATA_CHECK_EQ(entry(), 42);   /* b_val() + 1 = 41 + 1 */
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* A resolver that never resolves anything. */
static int ResolverNever(void* userData, const char* importerName, const char* importPath, StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;
    (void)importPath;
    (void)out;
    return 0;
}

STRATA_TEST(resolver_not_found_is_reported)
{
    /* When the resolver declines (returns 0), it is a hard error - there is
       no filesystem fallback while a resolver is installed. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverNever, NULL);

    StrataResult r = strataCompileString(c,
        "import ghost;\n"
        "int entry() { return 1; }\n",
        "main", STRATA_EMIT_C, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "cannot resolve import") != NULL);
    STRATA_CHECK(strstr(r.diagnostics, "ghost") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

/* Records the importerName passed to the resolver (copied, since the arena
   backing it is freed before the compile call returns). */
static char g_resolverImporterBuf[128];

static int ResolverRecord(void* userData, const char* importerName, const char* importPath, StrataResolvedModule* out)
{
    (void)userData;
    (void)importPath;

    if (importerName)
    {
        size_t n = strlen(importerName);
        if (n >= sizeof(g_resolverImporterBuf))
        {
            n = sizeof(g_resolverImporterBuf) - 1;
        }
        memcpy(g_resolverImporterBuf, importerName, n);
        g_resolverImporterBuf[n] = '\0';
    }

    out->text = "int child_val() { return 7; }";
    out->length = strlen(out->text);
    out->name = "child";
    return 1;
}

STRATA_TEST(resolver_receives_importer_name)
{
    /* The resolver is told the canonical name of the importing module, so a
       host can do relative-style resolution. For a top-level import in a
       string source, that name is the module name passed to the compile. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverRecord, NULL);
    g_resolverImporterBuf[0] = '\0';

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import child;\n"
        "int entry() { return child_val(); }\n",
        "myroot", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK(strcmp(g_resolverImporterBuf, "myroot") == 0);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* A resolver that overrides the on-disk weapons/pistol with a virtual one. */
static int ResolverVirtualPistol(void* userData, const char* importerName, const char* importPath,
                                 StrataResolvedModule* out)
{
    (void)userData;
    (void)importerName;

    if (strcmp(importPath, "weapons/pistol") == 0)
    {
        /* The disk version echoes its input (MakePistol(5).ammo == 5); this
           virtual one always reports 99, proving the resolver wins. */
        out->text = "struct Pistol { int ammo; };\n"
                    "Pistol MakePistol(int ammo) { return { .ammo = 99 }; }\n";
        out->length = strlen(out->text);
        out->name = "weapons/pistol";
        return 1;
    }

    return 0;
}

STRATA_TEST(resolver_overrides_filesystem_import)
{
    /* A file-compiled main (read from disk) whose import is served by the
       resolver instead of the filesystem: mixed disk-main + virtual imports. */
    char path[512];
    SamplePath("main.strata", path, sizeof(path));   /* import weapons/pistol; return MakePistol(5).ammo; */

    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverVirtualPistol, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
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
        STRATA_CHECK_EQ(entry(), 99);   /* virtual pistol, not the disk's 5 */
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
