#include "strata/strata.h"

#include "Codegen/CBackend.h"
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

#if STRATA_HAS_TCC
#include "Codegen/TccJit.h"
#endif

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
    };

#if STRATA_HAS_TCC
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
#endif

    StrataCompiler* strataCompilerCreate(void)
    {
        StrataCompiler* compiler = (StrataCompiler*)malloc(sizeof(StrataCompiler));
        compiler->arch = STRATA_ARCH_AUTO;
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

    static StrataResult BuildResult(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                    size_t sourceCount, StrataEmitKind emit, StrataEmitFlags emitFlags,
                                    const StrataArch arch)
    {
        StrataResult r = {0};

        const char* out = "";
        char* irOwned = NULL;

        CBackendEmitFlags backendEmitFlags = CEmitEnableSIMD;

        if ((emitFlags & STRATA_EMIT_NO_SIMD) != 0)
        {
            backendEmitFlags &= (~CEmitEnableSIMD);
        }

        if (!DiagHasErrors(diag) && mod)
        {
            if (emit == STRATA_EMIT_AST)
            {
                out = DumpAst(mod, arena);
            }
            else if (emit == STRATA_EMIT_C)
            {
                BuiltCModule result
                    = BuildCModuleWithSources(mod, diag, arena, sources, sourceCount, backendEmitFlags, arch);
                irOwned = DupString(result.source ? result.source : "");
                out = irOwned ? irOwned : "";
                BuiltCModuleDispose(&result);
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
        (void)c;

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
                         "imports are not supported when compiling from a string; use strataCompileFile");
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
            StrataResult r = {0};
            r.ok = 0;
            r.output = DupString("");
            r.diagnostics = DupString("null compiler or source");

            return r;
        }

        return CompileSource(c, source, strlen(source), moduleName ? moduleName : "strata_module", emit, emitFlags,
                             c->arch);
    }

    StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit, StrataEmitFlags emitFlags)
    {
        if (!c || !path)
        {
            StrataResult r = {0};
            r.ok = 0;
            r.output = DupString("");
            r.diagnostics = DupString("null compiler or path");

            return r;
        }

        Arena arena;
        arena_init(&arena, 0);

        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);

        ModuleLoader loader;
        ModuleLoaderInit(&loader, &arena, &diag);

        Module* mod = ModuleLoaderLoad(&loader, path);
        ResolveOverloads(mod, &diag, &arena);

        StrataResult r = BuildResult(mod, &diag, &arena, loader.sources, loader.sourceCount, emit, emitFlags, c->arch);

        AstDispose((Node*)mod);
        ModuleLoaderDispose(&loader);
        DiagnosticEngineFree(&diag);
        arena_free(&arena);

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

        if (errOut)
        {
            *errOut = DupString("LLVM backend not built");
        }

        return 0;
#else

    if (!inputPath || !outputPath)
    {
        if (errOut)
        {
            *errOut = DupString("null input or output path");
        }

        return 0;
    }

    Arena arena;
    arena_init(&arena, 0);

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    ModuleLoader loader;
    ModuleLoaderInit(&loader, &arena, &diag);

    Module* mod = ModuleLoaderLoad(&loader, inputPath);
    ResolveOverloads(mod, &diag, &arena);

    if (DiagHasErrors(&diag) || !mod)
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);

        if (errOut)
        {
            *errOut = DupString(diagText ? diagText : "compilation failed");
        }

        AstDispose((Node*)mod);
        ModuleLoaderDispose(&loader);
        DiagnosticEngineFree(&diag);

        arena_free(&arena);

        return 0;
    }

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false);

    if (DiagHasErrors(&diag))
    {
        char* diagText = DiagFormat(&diag, loader.sources, loader.sourceCount, &arena);

        if (errOut)
        {
            *errOut = DupString(diagText ? diagText : "code generation failed");
        }

        BuiltModuleDispose(&bm);
        AstDispose((Node*)mod);
        ModuleLoaderDispose(&loader);
        DiagnosticEngineFree(&diag);
        arena_free(&arena);

        return 0;
    }

    char* emitErr = NULL;
    int ok = EmitNativeFile(&bm, outputPath, assembly, &emitErr, NULL);

    if (!ok && errOut)
    {
        *errOut = DupString(emitErr ? emitErr : "emission failed");
    }

    BuiltModuleDispose(&bm);
    AstDispose((Node*)mod);
    ModuleLoaderDispose(&loader);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

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
        unsigned capabilities = STRATA_CAP_C_OUTPUT;
#if STRATA_HAS_LLVM
        capabilities |= STRATA_CAP_LLVM_IR | STRATA_CAP_LLVM_AOT;
#endif
#if STRATA_HAS_TCC
        capabilities |= STRATA_CAP_TCC_JIT;
#endif
        return capabilities;
    }

#if STRATA_HAS_TCC
    struct StrataJit
    {
        TccJit* jit;
        char* diagnostics;
    };

    static StrataJit* JitFromModule(Module* mod, DiagnosticEngine* diag, Arena* arena, const SourceManager* sources,
                                    size_t sourceCount, const char** errOut, const StrataArch arch)
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

        BuiltCModule bm = BuildCModuleWithSources(mod, diag, arena, sources, sourceCount, CEmitJIT, arch);

        if (DiagHasErrors(diag))
        {
            if (errOut)
            {
                char* allDiag = DiagFormat(diag, sources, sourceCount, arena);
                *errOut = ConcatOwned("codegen errors:\n", allDiag);
            }

            BuiltCModuleDispose(&bm);
            return NULL;
        }

        TccJit* jit = (TccJit*)malloc(sizeof(TccJit));
        TccJitInit(jit);

        char* err = NULL;
        if (!TccJitLoad(jit, &bm, &err))
        {
            if (errOut)
            {
                *errOut = ConcatOwned("JIT error: ", err ? err : "(unknown)");
            }

            free(err);
            TccJitDestroy(jit);
            free(jit);
            BuiltCModuleDispose(&bm);
            return NULL;
        }

        BuiltCModuleDispose(&bm);

        StrataJit* handle = (StrataJit*)calloc(1, sizeof(StrataJit));
        handle->jit = jit;
        handle->diagnostics = DupString(diagText);

        return handle;
    }

    static StrataJit* JitCompileString(StrataCompiler* c, const char* source, size_t sourceLen, const char* moduleName,
                                       const char** errOut, const StrataArch arch)
    {
        (void)c;

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
                         "imports are not supported when compiling from a string; use strataJitCompileFile");
        }

        ResolveOverloads(mod, &diag, &arena);

        StrataJit* handle = JitFromModule(mod, &diag, &arena, &src, 1, errOut, arch);

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
            if (errOut)
            {
                *errOut = DupString("null compiler or source");
            }

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
        arena_init(&arena, 0);

        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);

        ModuleLoader loader;
        ModuleLoaderInit(&loader, &arena, &diag);

        Module* mod = ModuleLoaderLoad(&loader, path);
        ResolveOverloads(mod, &diag, &arena);

        StrataJit* jit = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut, c->arch);

        AstDispose((Node*)mod);
        ModuleLoaderDispose(&loader);
        DiagnosticEngineFree(&diag);

        arena_free(&arena);

        return jit;
    }

    void* strataJitGetFunction(StrataJit* jit, const char* name)
    {
        if (!jit || !jit->jit || !name)
        {
            return NULL;
        }

        return TccJitGetAddress(jit->jit, name);
    }

    int strataJitCanInvokeIntVoid(StrataJit* jit, const char* name)
    {
        if (!jit || !jit->jit || !name)
        {
            return 0;
        }
        return TccJitCanInvokeIntVoid(jit->jit, name) ? 1 : 0;
    }

    int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
    {
        if (!jit || !jit->jit || !name || !fn)
        {
            return 0;
        }

        return TccJitAddSymbol(jit->jit, name, fn) ? 1 : 0;
    }

    size_t strataJitGetExternSymbolCount(StrataJit* jit)
    {
        if (!jit || !jit->jit)
        {
            return 0;
        }

        return TccJitExternCount(jit->jit);
    }

    const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index)
    {
        if (!jit || !jit->jit)
        {
            return NULL;
        }

        return TccJitExternName(jit->jit, index);
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

        if (jit->jit)
        {
            TccJitDestroy(jit->jit);
            free(jit->jit);
        }

        free(jit->diagnostics);
        free(jit);
    }

#else

struct StrataJit
{
    char unused;
};

static StrataJit* UnavailableJit(const char** errOut)
{
    if (errOut)
    {
        *errOut = DupString("JIT backend not built");
    }

    return NULL;
}

StrataJit* strataJitCompileString(StrataCompiler* c, const char* source, const char* moduleName, const char** errOut)
{
    (void)c;
    (void)source;
    (void)moduleName;
    return UnavailableJit(errOut);
}

StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut)
{
    (void)c;
    (void)path;
    return UnavailableJit(errOut);
}

void* strataJitGetFunction(StrataJit* jit, const char* name)
{
    (void)jit;
    (void)name;
    return NULL;
}

int strataJitCanInvokeIntVoid(StrataJit* jit, const char* name)
{
    (void)jit;
    (void)name;
    return 0;
}

int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
{
    (void)jit;
    (void)name;
    (void)fn;
    return 0;
}

size_t strataJitGetExternSymbolCount(StrataJit* jit)
{
    (void)jit;
    return 0;
}

const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index)
{
    (void)jit;
    (void)index;
    return NULL;
}

const char* strataJitDiagnostics(StrataJit* jit)
{
    (void)jit;
    return "";
}

void strataJitDestroy(StrataJit* jit)
{
    (void)jit;
}
#endif

    void strataFree(char* s)
    {
        free(s);
    }

#ifdef __cplusplus
}
#endif
