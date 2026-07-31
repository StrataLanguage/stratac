#include "strata/strata.h"

#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Import/ModuleLoader.h"
#include "strata/Codegen/LLVMCApi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct StrataCompiler
{
    char unused;
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

StrataCompiler* strataCompilerCreate(void)
{
    return (StrataCompiler*)malloc(sizeof(StrataCompiler));
}

void strataCompilerDestroy(StrataCompiler* c)
{
    free(c);
}

static StrataResult BuildResult(Module* mod, DiagnosticEngine* diag, Arena* arena,
                                const SourceManager* sources, size_t sourceCount, StrataEmitKind emit)
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
        else
        {
            CodegenResult result = GenerateLlvmIr(mod);
            irOwned = result.output;
            out = result.output ? result.output : "";
            if (!result.ok)
            {
                DiagError(diag, SRC_INVALID, "code generation failed");
            }
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

static StrataResult CompileSource(StrataCompiler* c, const char* source, size_t sourceLen,
                                  const char* moduleName, StrataEmitKind emit)
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

    StrataResult r = BuildResult(mod, &diag, &arena, &src, 1, emit);

    DiagnosticEngineFree(&diag);
    SourceManagerFree(&src);
    arena_free(&arena);

    return r;
}

StrataResult strataCompileString(StrataCompiler* c, const char* source,
                                const char* moduleName, StrataEmitKind emit)
{
    if (!c || !source)
    {
        StrataResult r = {0};
        r.ok = 0;
        r.output = DupString("");
        r.diagnostics = DupString("null compiler or source");

        return r;
    }

    return CompileSource(
        c,
        source,
        strlen(source),
        moduleName ? moduleName : "strata_module",
        emit);
}

StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit)
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

    StrataResult r = BuildResult(mod, &diag, &arena, loader.sources, loader.sourceCount, emit);

    ModuleLoaderDispose(&loader);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

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

const char* strataLLVMVersion(void)
{
    static char buf[32];
    unsigned maj = 0;
    unsigned min = 0;
    unsigned pat = 0;
    LLVMGetVersion(&maj, &min, &pat);
    snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);
    return buf;
}

struct StrataJit
{
    LLVMJit* jit;
    char* diagnostics;
};

static StrataJit* JitFromModule(Module* mod, DiagnosticEngine* diag, Arena* arena,
                                const SourceManager* sources, size_t sourceCount, const char** errOut)
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

    BuiltModule bm = BuildLlvmModule(mod, diag, arena, true);

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

    LLVMJit* jit = (LLVMJit*)malloc(sizeof(LLVMJit));
    LLVMJitInit(jit);

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
        return NULL;
    }

    BuiltModuleDispose(&bm);

    StrataJit* handle = (StrataJit*)calloc(1, sizeof(StrataJit));
    handle->jit = jit;
    handle->diagnostics = DupString(diagText);

    return handle;
}

static StrataJit* JitCompileString(StrataCompiler* c, const char* source, size_t sourceLen,
                                   const char* moduleName, const char** errOut)
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

    StrataJit* handle = JitFromModule(mod, &diag, &arena, &src, 1, errOut);

    DiagnosticEngineFree(&diag);
    SourceManagerFree(&src);
    arena_free(&arena);

    return handle;
}

StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                 const char* moduleName, const char** errOut)
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

    return JitCompileString(c, source, strlen(source),
                          moduleName ? moduleName : "strata_module", errOut);
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

    StrataJit* jit = JitFromModule(mod, &diag, &arena, loader.sources, loader.sourceCount, errOut);

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

    uint64_t addr = LLVMJitGetAddress(jit->jit, name);
    return addr ? (void*)(uintptr_t)addr : NULL;
}

int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
{
    if (!jit || !jit->jit || !name || !fn)
    {
        return 0;
    }

    return LLVMJitAddSymbol(jit->jit, name, fn) ? 1 : 0;
}

size_t strataJitGetExternSymbolCount(StrataJit* jit)
{
    if (!jit || !jit->jit)
    {
        return 0;
    }

    return jit->jit->m_externs.count;
}

const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index)
{
    if (!jit || !jit->jit)
    {
        return NULL;
    }

    if (index >= jit->jit->m_externs.count)
    {
        return NULL;
    }

    return (const char*)jit->jit->m_externs.items[index];
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
        LLVMJitDestroy(jit->jit);
        free(jit->jit);
    }

    free(jit->diagnostics);
    free(jit);
}

void strataFree(char* s)
{
    free(s);
}

#ifdef __cplusplus
}
#endif
