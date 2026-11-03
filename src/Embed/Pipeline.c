/* Shared compile-pipeline helpers: module loader setup and teardown, semantic analysis and reserved-name checks. */
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    char* ConcatOwned(const char* a, const char* b)
    {
        size_t na = strlen(a);
        size_t nb = strlen(b);
        char* buf = (char*)malloc(na + nb + 1);
        if (buf)
        {
            memcpy(buf, a, na);
            memcpy(buf + na, b, nb);
            buf[na + nb] = '\0';
        }
        return buf;
    }

    // Writes an error message into *errOut (owned), or no-ops when errOut is NULL.
    void SetErrOut(const char** errOut, const char* message, const char* fallback)
    {
        if (errOut)
        {
            *errOut = DupString(message ? message : fallback);
        }
    }

    // Sets up the per-compile arena/diagnostics/loader and binds the host
    // resolver (if any) so `import` directives are routed to it.
    void InitModuleLoader(Arena* arena, DiagnosticEngine* diag, ModuleLoader* loader, StrataCompiler* c)
    {
        ArenaInit(arena, 0);
        DiagnosticEngineInit(diag);
        ModuleLoaderInit(loader, arena, diag);
        ModuleLoaderSetResolver(loader, c->importResolver, c->importResolverUserData);
        ModuleLoaderSetImportDirs(loader, (const char* const*)c->importDirs, c->importDirCount);
    }

    // Releases everything owned by a ModuleLoader-based compile.
    void TeardownCompile(Module* mod, ModuleLoader* loader, DiagnosticEngine* diag, Arena* arena)
    {
        AstDispose((Node*)mod);
        ModuleLoaderDispose(loader);
        DiagnosticEngineFree(diag);
        ArenaFree(arena);
    }

    /* Symbols the runtime (or the generated module itself) defines. A user
       function with one of these names collides with it: in the JIT as a
       duplicate definition, in AOT at host link time. */
    static bool IsReservedRuntimeName(const char* name)
    {
        static const char* const kReserved[] = {
            "strata_alloc",  "strata_free",   "strata_panic",   "strata_oob",
            "strata_strdup", "strata_str_eq", "strata_cstrlen",
        };

        if (!name)
        {
            return false;
        }

        if (strncmp(name, "__strata_", 9) == 0)
        {
            return true;
        }

        for (size_t i = 0; i < ARRAY_COUNT(kReserved); i++)
        {
            if (strcmp(name, kReserved[i]) == 0)
            {
                return true;
            }
        }

        return false;
    }

    // The runtime entry points generated code calls but never defines: the host provides them.
    static bool IsHostRuntimeHook(const char* name)
    {
        return strcmp(name, "strata_alloc") == 0 || strcmp(name, "strata_free") == 0
               || strcmp(name, "strata_panic") == 0 || strcmp(name, "strata_oob") == 0;
    }

    static void CheckReservedNames(const Module* mod, DiagnosticEngine* diag)
    {
        if (!mod)
        {
            return;
        }

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            const FunctionDecl* fn = (const FunctionDecl*)VecGet((Vec*)&mod->functions, i);

            /* A module may supply the host hooks itself, as `@export` functions (a Strata-written runtime). */
            if (fn->isExport && IsHostRuntimeHook(fn->name))
            {
                continue;
            }

            if (!fn->isExtern && (IsReservedRuntimeName(fn->name) || IsReservedRuntimeName(fn->mangledName)))
            {
                DiagErrorFmt(diag, fn->base.range, "'%s' is a reserved Strata runtime name and cannot be defined",
                             fn->name);
            }
        }
    }

    // Semantic analysis plus the embedding-level checks every entry point shares.
    void AnalyzeModule(Module* mod, DiagnosticEngine* diag, Arena* arena)
    {
        ResolveOverloads(mod, diag, arena);
        CheckReservedNames(mod, diag);
    }

#ifdef __cplusplus
}
#endif
