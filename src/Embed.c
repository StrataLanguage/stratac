#include "strata/strata.h"

#include "Codegen/CodegenBackend.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Sema/ResolveOverloads.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Import/ModuleLoader.h"

#if STRATA_HAS_LLVM
#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMCApi.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    struct StrataCompiler
    {
        StrataArch arch;
        void* allocFn;   // optional host allocator for JIT mode
        void* freeFn;    // optional host deallocator for JIT mode
        StrataJitBackend jitBackend;
        StrataProfile profile; // JIT runtime checks (default: all on)

        StrataImportResolverFn importResolver;      // optional import resolver
        void* importResolverUserData;
    };

    static char* ConcatOwned(const char* a, const char* b)
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
    static void SetErrOut(const char** errOut, const char* message, const char* fallback)
    {
        if (errOut)
        {
            *errOut = DupString(message ? message : fallback);
        }
    }

    // Builds a zeroed StrataResult carrying a single error diagnostic.
    static StrataResult NullResult(const char* message)
    {
        StrataResult r = {0};
        r.ok = 0;
        r.output = DupString("");
        r.diagnostics = DupString(message ? message : "");
        return r;
    }

    // Sets up the per-compile arena/diagnostics/loader and binds the host
    // resolver (if any) so `import` directives are routed to it.
    static void InitModuleLoader(Arena* arena, DiagnosticEngine* diag, ModuleLoader* loader, StrataCompiler* c)
    {
        arena_init(arena, 0);
        DiagnosticEngineInit(diag);
        ModuleLoaderInit(loader, arena, diag);
        ModuleLoaderSetResolver(loader, c->importResolver, c->importResolverUserData);
    }

    // Releases everything owned by a ModuleLoader-based compile.
    static void TeardownCompile(Module* mod, ModuleLoader* loader, DiagnosticEngine* diag, Arena* arena)
    {
        AstDispose((Node*)mod);
        ModuleLoaderDispose(loader);
        DiagnosticEngineFree(diag);
        arena_free(arena);
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
        return compiler;
    }

    void strataCompilerDestroy(StrataCompiler* c)
    {
        free(c);
    }

    void strataSetArchitecture(StrataCompiler* c, StrataArch arch)
    {
        c->arch = arch;
    }

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

    StrataProfile strataProfileDefault(void)
    {
        StrataProfile p;
        p.boundsCheck = 1;
        p.nullExternCall = 1;
        return p;
    }

    void strataJitSetProfile(StrataCompiler* c, const StrataProfile* profile)
    {
        if (!c || !profile)
        {
            return;
        }

        c->profile = *profile;
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
            ResolveOverloads(mod, &diag, &arena);

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

        ResolveOverloads(mod, &diag, &arena);

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
        ResolveOverloads(mod, &diag, &arena);

        StrataResult r = BuildResult(mod, &diag, &arena, loader.sources, loader.sourceCount, emit, emitFlags, c->arch);

        TeardownCompile(mod, &loader, &diag, &arena);

        return r;
    }

    int strataCompileToObject(StrataCompiler* c, const char* inputPath, const char* outputPath, int assembly,
                              const char** errOut)
    {
        (void)c;

#if !STRATA_HAS_LLVM
    (void)inputPath;
    (void)outputPath;
    (void)assembly;

    SetErrOut(errOut, "LLVM backend not built", "");

    return 0;
#else

    if (!inputPath || !outputPath)
    {
        SetErrOut(errOut, "null input or output path", "");
        return 0;
    }

    Arena arena;
    DiagnosticEngine diag;
    ModuleLoader loader;
    InitModuleLoader(&arena, &diag, &loader, c);

    Module* mod = ModuleLoaderLoad(&loader, inputPath);
    ResolveOverloads(mod, &diag, &arena);

    if (DiagHasErrors(&diag) || !mod)
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);
        SetErrOut(errOut, diagText, "compilation failed");

        TeardownCompile(mod, &loader, &diag, &arena);

        return 0;
    }

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false, NULL);

    if (DiagHasErrors(&diag))
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);
        SetErrOut(errOut, diagText, "code generation failed");

        BuiltModuleDispose(&bm);
        TeardownCompile(mod, &loader, &diag, &arena);

        return 0;
    }

    char* emitErr = NULL;
    int ok = EmitNativeFile(&bm, outputPath, assembly, &emitErr, NULL);

    if (!ok)
    {
        SetErrOut(errOut, emitErr, "emission failed");
    }

    free(emitErr);

    BuiltModuleDispose(&bm);
    TeardownCompile(mod, &loader, &diag, &arena);

    return ok;
#endif
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

    const char* strataLLVMVersion(void)
    {
#if STRATA_HAS_LLVM
        static char buf[32];
        unsigned maj = 0;
        unsigned min = 0;
        unsigned pat = 0;
        LLVMGetVersion(&maj, &min, &pat);
        snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);

        return buf;
#else
        return "disabled";
#endif
    }

    unsigned strataCapabilities(void)
    {
        unsigned capabilities = 0;
#if STRATA_HAS_LLVM
        capabilities |= STRATA_CAP_LLVM_IR | STRATA_CAP_LLVM_AOT | STRATA_CAP_LLVM_JIT;
#endif
        return capabilities;
    }

    typedef enum
    {
        STRATA_JIT_KIND_NONE = 0,
        STRATA_JIT_KIND_LLVM,
    } StrataJitKind;

    struct StrataJit
    {
        StrataJitKind kind;
        void* backend;   // LLVMJit*, per kind; NULL if kind == NONE
        char* diagnostics;
#if STRATA_HAS_LLVM
        Vec llvmExports; // LlvmJitExport*, only populated when kind == STRATA_JIT_KIND_LLVM
#endif
    };

#if STRATA_HAS_LLVM
    typedef struct
    {
        char* name;
        bool isIntVoid;
    } LlvmJitExport;

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
                                        void* freeFn, const StrataProfile* profile)
    {
        BuiltModule bm = BuildLlvmModule(mod, diag, arena, true, profile);

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
            exp->isIntVoid = strcmp(fn->returnType.name, "int") == 0 && fn->params.count == 0;
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

        BuiltModuleDispose(&bm);

        handle->kind = STRATA_JIT_KIND_LLVM;
        handle->backend = jit;
        handle->diagnostics = DupString(diagText);

        return handle;
    }
#endif

    static StrataJit* JitFromModule(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                    size_t sourceCount, const char** errOut, const StrataArch arch, void* allocFn,
                                    void* freeFn, StrataJitBackend want, const StrataProfile* profile)
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
            return JitFromModuleLlvm(mod, diag, arena, sources, sourceCount, diagText, errOut, allocFn, freeFn,
                                     profile);
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
            ResolveOverloads(mod, &diag, &arena);

            StrataJit* handle = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut, arch,
                                              c->allocFn, c->freeFn, c->jitBackend, &c->profile);

            TeardownCompile(mod, &loader, &diag, &arena);

            return handle;
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
                         "imports are not supported when compiling from a string; use strataJitCompileFile or "
                         "strataSetImportResolver");
        }

        ResolveOverloads(mod, &diag, &arena);

        StrataJit* handle
            = JitFromModule(mod, &diag, &arena, &src, 1, errOut, arch, c->allocFn, c->freeFn, c->jitBackend,
                            &c->profile);

        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);

        arena_free(&arena);

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
        ResolveOverloads(mod, &diag, &arena);

        StrataJit* jit = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut, c->arch,
                                       c->allocFn, c->freeFn, c->jitBackend, &c->profile);

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
            return (void*)(uintptr_t)LLVMJitGetAddress((LLVMJit*)jit->backend, name);
        }
#endif
        return NULL;
    }

    int strataJitCanInvokeIntVoid(StrataJit* jit, const char* name)
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
        free(jit);
    }

    static StrataPanicHandler s_panicHandler = NULL;

    void strata_panic(const char* msg)
    {
        if (s_panicHandler)
        {
            s_panicHandler(msg);
        }
        else
        {
            fputs("strata panic: ", stderr);
            fputs(msg, stderr);
            fputc('\n', stderr);
            abort();
        }
    }

    void strata_oob(const char* msg)
    {
        // Reports a recoverable out-of-bounds access (LLVM JIT, boundsCheck on): the panic
        // handler (if any) is notified, but execution continues — the caller gets a dummy
        // element / the write is absorbed. A handler that longjmps still stops the program.
        if (s_panicHandler)
        {
            s_panicHandler(msg);
        }
        else
        {
            fputs("strata bounds violation: ", stderr);
            fputs(msg, stderr);
            fputc('\n', stderr);
        }
    }

    void strataSetPanicHandler(StrataPanicHandler handler)
    {
        s_panicHandler = handler;
    }

    void strataFree(char* s)
    {
        free(s);
    }

#ifdef __cplusplus
}
#endif
