/* Public compiler API: create and destroy, options, string and file compilation, result and string release. */
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Builds a zeroed StrataResult carrying a single error diagnostic.
    static StrataResult NullResult(const char* message)
    {
        StrataResult r = {0};
        r.ok = 0;
        r.output = DupString("");
        r.diagnostics = DupString(message ? message : "");
        return r;
    }

    static unsigned CountWarnings(const DiagnosticEngine* diag)
    {
        unsigned count = 0;

        for (size_t i = 0; i < diag->m_count; i++)
        {
            if (diag->m_diagnostics[i].severity == SevWarning)
            {
                count++;
            }
        }

        return count;
    }

    StrataCompiler* strataCompilerCreate(void)
    {
        StrataCompiler* compiler = (StrataCompiler*)malloc(sizeof(StrataCompiler));
        compiler->arch = STRATA_ARCH_AUTO;
        compiler->allocFn = NULL;
        compiler->freeFn = NULL;
        compiler->jitBackend = STRATA_JIT_BACKEND_AUTO;
        compiler->profile = strataProfileDefault();
        compiler->importResolver = NULL;
        compiler->importResolverUserData = NULL;
        compiler->symbolPrefix = NULL;
        compiler->importDirs = NULL;
        compiler->importDirCount = 0;
        return compiler;
    }

    void strataCompilerDestroy(StrataCompiler* c)
    {
        if (c)
        {
            free(c->symbolPrefix);

            for (size_t i = 0; i < c->importDirCount; i++)
            {
                free(c->importDirs[i]);
            }

            free(c->importDirs);
        }

        free(c);
    }

    int strataAddImportDir(StrataCompiler* c, const char* dir)
    {
        if (!c || !dir || !dir[0])
        {
            return 0;
        }

        char** grown = (char**)realloc(c->importDirs, (c->importDirCount + 1) * sizeof(char*));

        if (!grown)
        {
            return 0;
        }

        c->importDirs = grown;
        c->importDirs[c->importDirCount++] = DupString(dir);

        return 1;
    }

    int strataSetSymbolPrefix(StrataCompiler* c, const char* prefix)
    {
        if (!c)
        {
            return 0;
        }

        for (const char* at = prefix; at && *at; at++)
        {
            char ch = *at;
            bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';

            if (!valid)
            {
                return 0;
            }
        }

        free(c->symbolPrefix);
        c->symbolPrefix = (prefix && prefix[0]) ? DupString(prefix) : NULL;

        return 1;
    }

    void strataSetArchitecture(StrataCompiler* c, StrataArch arch)
    {
        c->arch = arch;
    }

    StrataProfile strataProfileDefault(void)
    {
        StrataProfile p;
        p.boundsCheck = 1;
        p.nullExternCall = 1;
        return p;
    }

    void strataSetImportResolver(StrataCompiler* c, StrataImportResolverFn resolver, void* userData)
    {
        if (!c)
        {
            return;
        }

        c->importResolver = resolver;
        c->importResolverUserData = userData;
    }

    static StrataResult BuildResult(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                    size_t sourceCount, StrataEmitKind emit, StrataEmitFlags emitFlags,
                                    const StrataArch arch)
    {
        StrataResult r = {0};

        const char* out = "";
        char* irOwned = NULL;

        if (emit == STRATA_EMIT_LLVM_IR && (emitFlags & STRATA_EMIT_NO_SIMD))
        {
            /* SIMD vector types always lower to native LLVM vectors; there is
               no scalarizing backend, so refuse rather than silently emit
               SIMD code. */
            DiagError(diag, SRC_INVALID,
                      "STRATA_EMIT_NO_SIMD is not supported: SIMD types always lower to native "
                      "vector instructions");
        }

        if (!DiagHasErrors(diag) && mod)
        {
            if (emit == STRATA_EMIT_AST)
            {
                out = DumpAst(mod, arena);
            }
            else if (emit == STRATA_EMIT_LLVM_IR)
            {
#if STRATA_HAS_LLVM
                CodegenResult result = GenerateLlvmIr(mod);
                irOwned = result.output;
                out = result.output ? result.output : "";
                if (!result.ok)
                {
                    DiagError(diag, SRC_INVALID, "code generation failed");
                }
#else
            DiagError(diag, SRC_INVALID, "LLVM backend not built");
#endif
            }
            else
            {
                DiagError(diag, SRC_INVALID, "unknown output kind");
            }
        }

        char* diagText = DiagFormat(diag, sources, sourceCount, arena);
        r.output = DupString(out);
        r.diagnostics = DupString(diagText);
        r.error_count = DiagErrorCount(diag);
        r.warning_count = CountWarnings(diag);
        r.ok = !DiagHasErrors(diag) ? 1 : 0;

        free(irOwned);
        return r;
    }

    static StrataResult CompileSource(StrataCompiler* c, const char* source, size_t sourceLen, const char* moduleName,
                                      StrataEmitKind emit, StrataEmitFlags emitFlags, const StrataArch arch)
    {
        if (c && c->importResolver)
        {
            // A resolver is installed: route through the module loader so that
            // `import X;` directives are resolved by the host.
            Arena arena;
            DiagnosticEngine diag;
            ModuleLoader loader;
            InitModuleLoader(&arena, &diag, &loader, c);

            Module* mod = ModuleLoaderLoadSource(&loader, moduleName, source, sourceLen);
            AnalyzeModule(mod, &diag, &arena);

            StrataResult r = BuildResult(mod, &diag, &arena, loader.sources, loader.sourceCount, emit, emitFlags, arch);

            TeardownCompile(mod, &loader, &diag, &arena);

            return r;
        }

        Arena arena;
        arena_init(&arena, 0);

        SourceManager src;
        SourceManagerInit(&src);
        SourceManagerSetSource(&src, source, sourceLen, moduleName);

        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);

        Lexer lex;
        LexerInit(&lex, src.m_text, src.m_textLen, &diag, 0);

        Parser parser;
        ParserInit(&parser, &lex, &diag, &arena, moduleName);

        Module* mod = ParserParseModule(&parser);

        if (mod && mod->imports.count > 0)
        {
            DiagErrorFmt(&diag, SRC_INVALID,
                         "imports are not supported when compiling from a string; use strataCompileFile or "
                         "strataSetImportResolver");
        }

        AnalyzeModule(mod, &diag, &arena);

        StrataResult r = BuildResult(mod, &diag, &arena, &src, 1, emit, emitFlags, arch);

        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);
        arena_free(&arena);

        return r;
    }

    StrataResult strataCompileString(StrataCompiler* c, const char* source, const char* moduleName, StrataEmitKind emit,
                                     StrataEmitFlags emitFlags)
    {
        if (!c || !source)
        {
            return NullResult("null compiler or source");
        }

        return CompileSource(c, source, strlen(source), moduleName ? moduleName : "strata_module", emit, emitFlags,
                             c->arch);
    }

    StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit, StrataEmitFlags emitFlags)
    {
        if (!c || !path)
        {
            return NullResult("null compiler or path");
        }

        Arena arena;
        DiagnosticEngine diag;
        ModuleLoader loader;
        InitModuleLoader(&arena, &diag, &loader, c);

        Module* mod = ModuleLoaderLoad(&loader, path);
        AnalyzeModule(mod, &diag, &arena);

        StrataResult r = BuildResult(mod, &diag, &arena, loader.sources, loader.sourceCount, emit, emitFlags, c->arch);

        TeardownCompile(mod, &loader, &diag, &arena);

        return r;
    }

    void strataResultFree(StrataResult* r)
    {
        if (!r)
        {
            return;
        }

        free((void*)r->output);
        free((void*)r->diagnostics);

        r->output = NULL;
        r->diagnostics = NULL;
    }

    void strataFree(char* s)
    {
        free(s);
    }

#ifdef __cplusplus
}
#endif
