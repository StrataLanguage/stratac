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

#ifdef STRATA_ENABLE_LLVM
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "strata/Codegen/LLVMCApi.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct StrataCompiler
{
    char unused;
};

static char* DupCString(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p)
    {
        memcpy(p, s, n);
    }
    return p;
}

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

static char* ReadFile(const char* path, size_t* outLen)
{
    FILE* in = fopen(path, "rb");
    if (!in)
    {
        return NULL;
    }

    if (fseek(in, 0, SEEK_END) != 0)
    {
        fclose(in);
        return NULL;
    }

    long size = ftell(in);
    if (size < 0)
    {
        fclose(in);
        return NULL;
    }

    rewind(in);

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(in);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, in);
    fclose(in);

    buf[n] = '\0';
    if (outLen)
    {
        *outLen = n;
    }
    return buf;
}

static const char* Basename(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }
    return base;
}

StrataCompiler* strataCompilerCreate(void)
{
    return (StrataCompiler*)malloc(sizeof(StrataCompiler));
}

void strataCompilerDestroy(StrataCompiler* c)
{
    free(c);
}

static StrataResult CompileSource(StrataCompiler* c, const char* source, size_t sourceLen,
                                 const char* moduleName, StrataEmitKind emit)
{
    (void)c;

    StrataResult r;
    memset(&r, 0, sizeof(r));
    r.ok = 0;
    r.output = DupCString("");
    r.diagnostics = DupCString("");

    Arena arena;
    arena_init(&arena, 0);

    SourceManager src;
    SourceManagerInit(&src);
    SourceManagerSetSource(&src, source, sourceLen, moduleName);

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    Lexer lex;
    LexerInit(&lex, src.m_text, src.m_textLen, &diag);

    Parser parser;
    ParserInit(&parser, &lex, &diag, &arena, moduleName);

    Module* mod = ParserParseModule(&parser);

    ResolveOverloads(mod, &diag, &arena);

    char* diagText = DiagFormat(&diag, &src, &arena);

    const char* out = "";
    char* irOwned = NULL;

    if (!DiagHasErrors(&diag) && mod)
    {
        if (emit == STRATA_EMIT_AST)
        {
            out = DumpAst(mod, &arena);
        }
        else
        {
            CodegenResult result = GenerateLlvmIr(mod);
            irOwned = result.output;
            out = result.output ? result.output : "";
            if (!result.ok)
            {
                DiagError(&diag, SRC_INVALID, "code generation failed");
            }
        }
    }

    free((void*)r.output);
    free((void*)r.diagnostics);
    r.output = DupCString(out);
    r.diagnostics = DupCString(diagText);
    r.error_count = DiagErrorCount(&diag);
    r.ok = !DiagHasErrors(&diag) ? 1 : 0;

    free(irOwned);
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
        StrataResult r;
        memset(&r, 0, sizeof(r));
        r.ok = 0;
        r.output = DupCString("");
        r.diagnostics = DupCString("null compiler or source");
        return r;
    }

    return CompileSource(c, source, strlen(source),
                        moduleName ? moduleName : "strata_module", emit);
}

StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit)
{
    if (!c || !path)
    {
        StrataResult r;
        memset(&r, 0, sizeof(r));
        r.ok = 0;
        r.output = DupCString("");
        r.diagnostics = DupCString("null compiler or path");
        return r;
    }

    size_t fileLen = 0;
    char* source = ReadFile(path, &fileLen);
    if (!source)
    {
        StrataResult r;
        memset(&r, 0, sizeof(r));
        r.ok = 0;
        r.output = DupCString("");
        r.diagnostics = ConcatOwned("cannot open file: ", path);
        r.error_count = 1;
        return r;
    }

    const char* moduleName = Basename(path);
    StrataResult r = CompileSource(c, source, fileLen, moduleName, emit);
    free(source);
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
#ifdef STRATA_ENABLE_LLVM
    static char buf[32];
    unsigned maj = 0;
    unsigned min = 0;
    unsigned pat = 0;
    LLVMGetVersion(&maj, &min, &pat);
    snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);
    return buf;
#else
    return "0.0.0";
#endif
}

struct StrataJit
{
#ifdef STRATA_ENABLE_LLVM
    LLVMJit* jit;
#endif
    char* diagnostics;
};

#ifdef STRATA_ENABLE_LLVM
static StrataJit* JitCompile(StrataCompiler* c, const char* source, size_t sourceLen,
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
    LexerInit(&lex, src.m_text, src.m_textLen, &diag);

    Parser parser;
    ParserInit(&parser, &lex, &diag, &arena, moduleName);

    Module* mod = ParserParseModule(&parser);

    ResolveOverloads(mod, &diag, &arena);

    char* diagText = DiagFormat(&diag, &src, &arena);

    if (DiagHasErrors(&diag) || !mod)
    {
        if (errOut)
        {
            *errOut = ConcatOwned("parse errors:\n", diagText);
        }

        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);
        arena_free(&arena);
        return NULL;
    }

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, true);

    if (DiagHasErrors(&diag))
    {
        if (errOut)
        {
            char* allDiag = DiagFormat(&diag, &src, &arena);
            *errOut = ConcatOwned("codegen errors:\n", allDiag);
        }

        BuiltModuleDispose(&bm);
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);
        arena_free(&arena);
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
        DiagnosticEngineFree(&diag);
        SourceManagerFree(&src);
        arena_free(&arena);
        return NULL;
    }

    BuiltModuleDispose(&bm);

    StrataJit* handle = (StrataJit*)calloc(1, sizeof(StrataJit));
    handle->jit = jit;
    handle->diagnostics = DupCString(diagText);

    DiagnosticEngineFree(&diag);
    SourceManagerFree(&src);
    arena_free(&arena);

    return handle;
}
#endif

StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                 const char* moduleName, const char** errOut)
{
#ifdef STRATA_ENABLE_LLVM
    if (errOut)
    {
        *errOut = NULL;
    }

    if (!c || !source)
    {
        if (errOut)
        {
            *errOut = DupCString("null compiler or source");
        }

        return NULL;
    }

    return JitCompile(c, source, strlen(source),
                     moduleName ? moduleName : "strata_module", errOut);
#else
    if (errOut)
    {
        *errOut = DupCString("JIT unavailable: strata built without LLVM");
    }

    return NULL;
#endif
}

StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut)
{
#ifdef STRATA_ENABLE_LLVM
    if (errOut)
    {
        *errOut = NULL;
    }

    if (!c || !path)
    {
        if (errOut)
        {
            *errOut = DupCString("null compiler or path");
        }

        return NULL;
    }

    size_t fileLen = 0;
    char* source = ReadFile(path, &fileLen);
    if (!source)
    {
        if (errOut)
        {
            *errOut = ConcatOwned("cannot open file: ", path);
        }

        return NULL;
    }

    const char* moduleName = Basename(path);
    StrataJit* jit = JitCompile(c, source, fileLen, moduleName, errOut);
    free(source);
    return jit;
#else
    if (errOut)
    {
        *errOut = DupCString("JIT unavailable: strata built without LLVM");
    }

    return NULL;
#endif
}

void* strataJitGetFunction(StrataJit* jit, const char* name)
{
#ifdef STRATA_ENABLE_LLVM
    if (!jit || !jit->jit || !name)
    {
        return NULL;
    }

    uint64_t addr = LLVMJitGetAddress(jit->jit, name);
    return addr ? (void*)(uintptr_t)addr : NULL;
#else
    (void)jit;
    (void)name;
    return NULL;
#endif
}

int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
{
#ifdef STRATA_ENABLE_LLVM
    if (!jit || !jit->jit || !name || !fn)
    {
        return 0;
    }

    return LLVMJitAddSymbol(jit->jit, name, fn) ? 1 : 0;
#else
    (void)jit;
    (void)name;
    (void)fn;
    return 0;
#endif
}

size_t strataJitGetExternSymbolCount(StrataJit* jit)
{
#ifdef STRATA_ENABLE_LLVM
    if (!jit || !jit->jit)
    {
        return 0;
    }

    return jit->jit->m_externs.count;
#else
    (void)jit;
    return 0;
#endif
}

const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index)
{
#ifdef STRATA_ENABLE_LLVM
    if (!jit || !jit->jit)
    {
        return NULL;
    }

    if (index >= jit->jit->m_externs.count)
    {
        return NULL;
    }

    return (const char*)jit->jit->m_externs.items[index];
#else
    (void)jit;
    (void)index;
    return NULL;
#endif
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

#ifdef STRATA_ENABLE_LLVM
    if (jit->jit)
    {
        LLVMJitDestroy(jit->jit);
        free(jit->jit);
    }
#endif

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
