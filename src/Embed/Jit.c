/* Public JIT API: in-memory compilation, symbol lookup and binding, diagnostics and destruction. */
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    void strataJitSetAllocFreeFunctions(StrataCompiler* c, void* allocFn, void* freeFn)
    {
        if (!c)
        {
            return;
        }

        c->allocFn = allocFn;
        c->freeFn = freeFn;
    }

    void strataJitSetBackend(StrataCompiler* c, StrataJitBackend backend)
    {
        if (!c)
        {
            return;
        }

        c->jitBackend = backend;
    }

    void strataJitSetProfile(StrataCompiler* c, const StrataProfile* profile)
    {
        if (!c || !profile)
        {
            return;
        }

        c->profile = *profile;
    }

#if STRATA_HAS_LLVM
    // Frees the llvmExports list (names, entries, and the items array).
    static void FreeJitExports(StrataJit* jit)
    {
        for (size_t i = 0; i < jit->llvmExports.count; i++)
        {
            LlvmJitExport* exp = (LlvmJitExport*)jit->llvmExports.items[i];
            free(exp->name);
            free(exp);
        }
        free(jit->llvmExports.items);
    }
#endif

    static StrataJit* UnavailableJit(const char** errOut)
    {
        if (errOut)
        {
            *errOut = DupString("JIT backend not built");
        }

        return NULL;
    }

    static StrataJitKind ResolveJitKind(StrataJitBackend want)
    {
#if STRATA_HAS_LLVM
        if (want == STRATA_JIT_BACKEND_LLVM || want == STRATA_JIT_BACKEND_AUTO)
        {
            return STRATA_JIT_KIND_LLVM;
        }
#else
    (void)want;
#endif
        return STRATA_JIT_KIND_NONE;
    }

#if STRATA_HAS_LLVM
    static StrataJit* JitFromModuleLlvm(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                        size_t sourceCount, const char* diagText, const char** errOut, void* allocFn,
                                        void* freeFn, const StrataProfile* profile, const char* symbolPrefix)
    {
        BuiltModule bm = BuildLlvmModuleEx(mod, diag, arena, true, profile, symbolPrefix);

        if (DiagHasErrors(diag))
        {
            if (errOut)
            {
                char* allDiag = DiagFormat(diag, sources, sourceCount, arena);
                *errOut = ConcatOwned("codegen errors:\n", allDiag);
            }

            BuiltModuleDispose(&bm);
            return NULL;
        }

        StrataJit* handle = (StrataJit*)calloc(1, sizeof(StrataJit));
        VecInit(&handle->llvmExports);

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);

            if (fn->isExtern)
            {
                continue;
            }

            LlvmJitExport* exp = (LlvmJitExport*)malloc(sizeof(LlvmJitExport));
            exp->name = DupString(fn->mangledName);
            /* The user-visible signature only; every non-extern function
               additionally takes a hidden leading context pointer (see
               LLVMModuleBuilder.c). */
            exp->isIntVoid = fn->returnType.primitiveType == PrimInt && fn->params.count == 0;
            VecPush(&handle->llvmExports, exp);
        }

        LLVMJit* jit = (LLVMJit*)malloc(sizeof(LLVMJit));
        LLVMJitInit(jit);

        if (allocFn || freeFn)
        {
            LLVMJitSetAllocFree(jit, allocFn, freeFn);
        }

        char* err = NULL;
        if (!LLVMJitLoad(jit, &bm, &err))
        {
            if (errOut)
            {
                *errOut = ConcatOwned("JIT error: ", err ? err : "(unknown)");
            }

            free(err);
            LLVMJitDestroy(jit);
            free(jit);
            BuiltModuleDispose(&bm);

            FreeJitExports(handle);
            free(handle);

            return NULL;
        }

        handle->typeMetadata = bm.typeMetadata;
        handle->typeMetadataSize = bm.typeMetadataSize;
        bm.typeMetadata = NULL;

        BuiltModuleDispose(&bm);

        handle->kind = STRATA_JIT_KIND_LLVM;
        handle->backend = jit;
        handle->diagnostics = DupString(diagText);
        handle->symbolPrefix = symbolPrefix ? DupString(symbolPrefix) : NULL;

        return handle;
    }
#endif

    static StrataJit* JitFromModule(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                    size_t sourceCount, const char** errOut, const StrataArch arch, void* allocFn,
                                    void* freeFn, StrataJitBackend want, const StrataProfile* profile,
                                    const char* symbolPrefix)
    {
        char* diagText = DiagFormat(diag, sources, sourceCount, arena);

        if (DiagHasErrors(diag) || !mod)
        {
            if (errOut)
            {
                *errOut = ConcatOwned("parse errors:\n", diagText);
            }

            return NULL;
        }

        switch (ResolveJitKind(want))
        {
#if STRATA_HAS_LLVM
        case STRATA_JIT_KIND_LLVM:
            return JitFromModuleLlvm(mod, diag, arena, sources, sourceCount, diagText, errOut, allocFn, freeFn, profile,
                                     symbolPrefix);
#endif
        default:
            return UnavailableJit(errOut);
        }
    }

    static StrataJit* JitCompileString(StrataCompiler* c, const char* source, size_t sourceLen, const char* moduleName,
                                       const char** errOut, const StrataArch arch)
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

            StrataJit* handle = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut, arch,
                                              c->allocFn, c->freeFn, c->jitBackend, &c->profile, c->symbolPrefix);

            TeardownCompile(mod, &loader, &diag, &arena);

            return handle;
        }

        Arena arena;
        ArenaInit(&arena, 0);

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
                         "imports are not supported when compiling from a string; use strataJitCompileFile or "
                         "strataSetImportResolver");
        }

        AnalyzeModule(mod, &diag, &arena);

        StrataJit* handle = JitFromModule(mod, &diag, &arena, &src, 1, errOut, arch, c->allocFn, c->freeFn,
                                          c->jitBackend, &c->profile, c->symbolPrefix);

        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);

        ArenaFree(&arena);

        return handle;
    }

    StrataJit* strataJitCompileString(StrataCompiler* c, const char* source, const char* moduleName,
                                      const char** errOut)
    {
        if (errOut)
        {
            *errOut = NULL;
        }

        if (!c || !source)
        {
            SetErrOut(errOut, "null compiler or source", "");
            return NULL;
        }

        return JitCompileString(c, source, strlen(source), moduleName ? moduleName : "strata_module", errOut, c->arch);
    }

    StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut)
    {
        if (errOut)
        {
            *errOut = NULL;
        }

        if (!c || !path)
        {
            if (errOut)
            {
                *errOut = DupString("null compiler or path");
            }

            return NULL;
        }

        Arena arena;
        DiagnosticEngine diag;
        ModuleLoader loader;
        InitModuleLoader(&arena, &diag, &loader, c);

        Module* mod = ModuleLoaderLoad(&loader, path);
        AnalyzeModule(mod, &diag, &arena);

        StrataJit* jit = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut, c->arch,
                                       c->allocFn, c->freeFn, c->jitBackend, &c->profile, c->symbolPrefix);

        TeardownCompile(mod, &loader, &diag, &arena);

        return jit;
    }

    void* strataJitGetFunction(StrataJit* jit, const char* name)
    {
        if (!jit || !jit->backend || !name)
        {
            return NULL;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            if (jit->symbolPrefix)
            {
                char* prefixed = ConcatOwned(jit->symbolPrefix, name);
                void* address = prefixed ? (void*)(uintptr_t)LLVMJitGetAddress((LLVMJit*)jit->backend, prefixed) : NULL;
                free(prefixed);

                if (address)
                {
                    return address;
                }
            }

            return (void*)(uintptr_t)LLVMJitGetAddress((LLVMJit*)jit->backend, name);
        }
#endif
        return NULL;
    }

    const void* strataJitGetTypeMetadata(StrataJit* jit, size_t* outSize)
    {
        if (outSize)
        {
            *outSize = jit ? jit->typeMetadataSize : 0;
        }

        return jit ? jit->typeMetadata : NULL;
    }

    int strataJitHasIntVoidSignature(StrataJit* jit, const char* name)
    {
        if (!jit || !jit->backend || !name)
        {
            return 0;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            for (size_t i = 0; i < jit->llvmExports.count; i++)
            {
                const LlvmJitExport* exp = (const LlvmJitExport*)jit->llvmExports.items[i];

                if (strcmp(exp->name, name) == 0)
                {
                    return exp->isIntVoid ? 1 : 0;
                }
            }
        }
#endif
        return 0;
    }

    int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
    {
        if (!jit || !jit->backend || !name || !fn)
        {
            return 0;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            return LLVMJitAddSymbol((LLVMJit*)jit->backend, name, fn) ? 1 : 0;
        }
#endif
        return 0;
    }

    size_t strataJitGetExternSymbolCount(StrataJit* jit)
    {
        if (!jit || !jit->backend)
        {
            return 0;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            return LLVMJitExternCount((LLVMJit*)jit->backend);
        }
#endif
        return 0;
    }

    const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index)
    {
        if (!jit || !jit->backend)
        {
            return NULL;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            return LLVMJitExternName((LLVMJit*)jit->backend, index);
        }
#endif
        return NULL;
    }

    const char* strataJitDiagnostics(StrataJit* jit)
    {
        if (!jit)
        {
            return "";
        }

        return jit->diagnostics ? jit->diagnostics : "";
    }

    void strataJitDestroy(StrataJit* jit)
    {
        if (!jit)
        {
            return;
        }
#if STRATA_HAS_LLVM
        if (jit->kind == STRATA_JIT_KIND_LLVM)
        {
            if (jit->backend)
            {
                LLVMJitDestroy((LLVMJit*)jit->backend);
                free(jit->backend);
            }

            FreeJitExports(jit);
        }
#endif

        free(jit->diagnostics);
        free(jit->symbolPrefix);
        free(jit->typeMetadata);
        free(jit);
    }

#ifdef __cplusplus
}
#endif
