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
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR, 0);

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
                                         "inline", STRATA_EMIT_LLVM_IR, 0);

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
    StrataResult r = strataCompileFile(c, path, STRATA_EMIT_LLVM_IR, 0);

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
        "main", STRATA_EMIT_LLVM_IR, 0);

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

/* ================= Cross-module types & functions ================= */

/* Serves a virtual "shapes" module: a value struct plus free functions that
   build and consume it. Exercises struct type sharing, construction, field
   access, imported functions, and returning/consuming imported structs. */
static int ResolverShapes(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "shapes") == 0)
    {
        out->text =
            "struct Vec2 { int x; int y; };\n"
            "Vec2 make_vec2(int x, int y) { return { x, y }; }\n"
            "int dot(Vec2 a, Vec2 b) { return a.x*b.x + a.y*b.y; }\n"
            "Vec2 unit() { return { 1, 1 }; }\n";
        out->length = strlen(out->text);
        out->name = "shapes";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_imports_struct_type_and_calls_function)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverShapes, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import shapes;\n"
        "int entry() { Vec2 v = make_vec2(3, 4); return dot(v, v); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 25);  /* 3*3 + 4*4 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(resolver_imported_struct_used_in_local_function)
{
    /* The imported struct is used as a parameter type and return type of a
       function DEFINED in the importing module. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverShapes, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import shapes;\n"
        "int norm_sq(Vec2 v) { return v.x*v.x + v.y*v.y; }\n"
        "int entry() { Vec2 v = { 5, 12 }; return norm_sq(v); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 169);  /* 25 + 144 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(resolver_imported_function_returns_struct)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverShapes, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import shapes;\n"
        "int entry() { Vec2 u = unit(); return u.x + u.y; }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 2);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* Serves a "geom" module with a point struct, to test passing imported structs
   as parameters and building arrays of imported element types. */
static int ResolverGeom(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "geom") == 0)
    {
        out->text =
            "struct Pt { int x; int y; };\n"
            "int manhattan(Pt p) { return p.x + p.y; }\n";
        out->length = strlen(out->text);
        out->name = "geom";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_imported_struct_passed_as_param)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverGeom, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import geom;\n"
        "int entry() { Pt p = { 10, 20 }; return manhattan(p); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 30);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(resolver_array_of_imported_struct_type)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverGeom, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import geom;\n"
        "int entry() { Pt[] pts = { Pt{1, 2}, Pt{3, 4} }; return pts[0].x + pts[1].y; }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 5);  /* 1 + 4 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* Deep three-module call chain: c -> b -> a. */
static int ResolverChain(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "a") == 0)
    {
        out->text = "int a_val() { return 1; }\n";
        out->length = strlen(out->text);
        out->name = "a";
        return 1;
    }
    if (strcmp(path, "b") == 0)
    {
        out->text = "import a;\nint b_val() { return a_val() + 10; }\n";
        out->length = strlen(out->text);
        out->name = "b";
        return 1;
    }
    if (strcmp(path, "c") == 0)
    {
        out->text = "import b;\nint c_val() { return b_val() + 100; }\n";
        out->length = strlen(out->text);
        out->name = "c";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_deep_call_chain_across_modules)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverChain, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import c;\n"
        "int entry() { return c_val(); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 111);  /* 1 + 10 + 100 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* Two modules that import each other and share a base function. */
static int ResolverMutual(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "a") == 0)
    {
        out->text = "import b;\n"
                    "int a_only() { return b_only() * 2; }\n"
                    "int shared_base() { return 1; }\n";
        out->length = strlen(out->text);
        out->name = "a";
        return 1;
    }
    if (strcmp(path, "b") == 0)
    {
        out->text = "import a;\n"
                    "int b_only() { return shared_base() + 3; }\n";
        out->length = strlen(out->text);
        out->name = "b";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_mutual_import_with_shared_function)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverMutual, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import a;\n"
        "import b;\n"
        "int entry() { return a_only() + b_only(); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 12);  /* a_only=8, b_only=4 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* A module that forward-declares a struct and declares host externs against
   it - the importing module supplies the definition. */
static int ResolverReturnParamSdk(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "sdk") == 0)
    {
        out->text = "struct Foo;\n"
                    "extern void GetFoo(return Foo f);\n"
                    "extern int UseFoo(Foo f);\n";
        out->length = strlen(out->text);
        out->name = "sdk";
        return 1;
    }
    if (strcmp(path, "sdkdef") == 0)
    {
        out->text = "struct Foo { int x; };\n"
                    "extern void GetFoo(return Foo f);\n"
                    "extern int UseFoo(Foo f);\n";
        out->length = strlen(out->text);
        out->name = "sdkdef";
        return 1;
    }
    return 0;
}

typedef struct
{
    int v;
} HostImportedFoo;

static void HostSdkGetFoo(HostImportedFoo* f)
{
    f->v = 40;
}

static int HostSdkUseFoo(HostImportedFoo* f)
{
    return f->v + 1;
}

STRATA_TEST(resolver_return_param_forward_decl_import)
{
    /* sdk forward-declares `struct Foo;` and declares `GetFoo(return Foo)`;
       the importing module defines Foo and calls it. The declaration
       survives the merge with the definition. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverReturnParamSdk, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import sdk;\n"
        "struct Foo { int x; };\n"
        "int entry() { Foo f = GetFoo(); return UseFoo(f) + f.x; }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "GetFoo", (void*)&HostSdkGetFoo), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "UseFoo", (void*)&HostSdkUseFoo), 1);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 81);  /* UseFoo(40)+1 + 40 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(resolver_return_param_forward_decl_import_reverse)
{
    /* The other direction: the importing module forward-declares Foo while
       the imported module defines it. */
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverReturnParamSdk, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import sdkdef;\n"
        "struct Foo;\n"
        "int entry() { Foo f = GetFoo(); return UseFoo(f) + f.x; }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "GetFoo", (void*)&HostSdkGetFoo), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "UseFoo", (void*)&HostSdkUseFoo), 1);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 81);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* A module exposing a global that the importer reads directly and via a fn. */
static int ResolverGlobals(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "cfg") == 0)
    {
        out->text = "int base = 100;\n"
                    "int base_twice() { return base * 2; }\n";
        out->length = strlen(out->text);
        out->name = "cfg";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_global_shared_across_modules)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverGlobals, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import cfg;\n"
        "int entry() { return base + base_twice(); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 300);  /* 100 + 200 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* Overloaded functions imported from another module. */
static int ResolverMath(void* ud, const char* importer, const char* path, StrataResolvedModule* out)
{
    (void)ud;
    (void)importer;
    if (strcmp(path, "math") == 0)
    {
        out->text = "int sum(int a, int b) { return a + b; }\n"
                    "int sum(int a, int b, int c) { return a + b + c; }\n";
        out->length = strlen(out->text);
        out->name = "math";
        return 1;
    }
    return 0;
}

STRATA_TEST(resolver_overloaded_function_imported)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverMath, NULL);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "import math;\n"
        "int entry() { return sum(1, 2) + sum(1, 2, 3); }\n",
        "main", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit) { printf("  JIT failed: %s\n", err ? err : "(none)"); strataFree((char*)err); strataCompilerDestroy(c); return; }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry) STRATA_CHECK_EQ(entry(), 9);  /* 3 + 6 */

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* ================= Cross-module error reporting ================= */

#if STRATA_TEST_HAS_LLVM
STRATA_TEST(resolver_unknown_type_from_import_is_error)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverShapes, NULL);

    StrataResult r = strataCompileString(c,
        "import shapes;\n"
        "int entry() { OtherT x; return 0; }\n",
        "main", STRATA_EMIT_LLVM_IR, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "OtherT") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(resolver_unknown_function_from_import_is_error)
{
    StrataCompiler* c = strataCompilerCreate();
    strataSetImportResolver(c, &ResolverShapes, NULL);

    StrataResult r = strataCompileString(c,
        "import shapes;\n"
        "int entry() { return missing_fn(); }\n",
        "main", STRATA_EMIT_LLVM_IR, 0);

    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(strstr(r.diagnostics, "missing_fn") != NULL);

    strataResultFree(&r);
    strataCompilerDestroy(c);
}
#endif
