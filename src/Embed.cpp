// Strata compiler: implementation of the public C embedding API (strata.h).
#include "strata/strata.h"

#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"

#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <cstdio>

#if defined(STRATA_ENABLE_LLVM)
#include "strata/Codegen/LLVMCApi.h"
#endif

struct StrataCompiler {
    bool useLLVM = true;
};

extern "C" {

StrataCompiler* strataCompilerCreate(void) {
    try { return new StrataCompiler{}; }
    catch (...) { return nullptr; }
}

void strataCompilerDestroy(StrataCompiler* c) { delete c; }

void strataCompilerUseLLVM(StrataCompiler* c, int enabled) {
    if (c) c->useLLVM = enabled != 0;
}

static char* dupCString(const std::string& s) {
    char* p = new (std::nothrow) char[s.size() + 1];
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

static StrataResult compileSource(StrataCompiler* c, std::string source,
                                  std::string moduleName, StrataEmitKind emit) {
    StrataResult r{};
    r.ok = 0;
    r.output = dupCString("");
    r.diagnostics = dupCString("");

    strata::SourceManager src;
    src.setSource(std::move(source), moduleName);

    strata::DiagnosticEngine diag;
    strata::Lexer lex(src.source(), diag);
    strata::Parser parser(lex, diag, moduleName);
    auto mod = parser.parseModule();

    std::string diagText = diag.format(src);
    std::string out;

    if (!diag.hasErrors() && mod) {
        if (emit == STRATA_EMIT_AST) {
            out = strata::dumpAST(*mod);
        } else {
            std::unique_ptr<strata::CodegenBackend> backend;
            if (c && c->useLLVM) backend = strata::createLLVMBackend();
            if (!backend) backend = strata::createTextBackend();
            auto result = backend->generate(*mod);
            out = result.output;
            if (!result.ok) {
                diag.error({}, "code generation failed");
            }
        }
    }

    delete[] r.output;
    delete[] r.diagnostics;
    r.output = dupCString(out);
    r.diagnostics = dupCString(diagText);
    r.error_count = diag.errorCount();
    r.ok = (!diag.hasErrors()) ? 1 : 0;
    return r;
}

StrataResult strataCompileString(StrataCompiler* c, const char* source,
                                 const char* moduleName, StrataEmitKind emit) {
    if (!c || !source) {
        StrataResult r{};
        r.ok = 0;
        r.output = dupCString("");
        r.diagnostics = dupCString("null compiler or source");
        return r;
    }
    return compileSource(c, std::string(source),
                         moduleName ? std::string(moduleName) : std::string("strata_module"),
                         emit);
}

StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit) {
    if (!c || !path) {
        StrataResult r{};
        r.ok = 0;
        r.output = dupCString("");
        r.diagnostics = dupCString("null compiler or path");
        return r;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        StrataResult r{};
        r.ok = 0;
        r.output = dupCString("");
        std::string m = std::string("cannot open file: ") + path;
        r.diagnostics = dupCString(m);
        r.error_count = 1;
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string base = path;
    // Use the file name (without path) as the module name.
    auto slash = base.find_last_of("/\\");
    std::string moduleName = (slash != std::string::npos) ? base.substr(slash + 1) : base;
    return compileSource(c, ss.str(), moduleName, emit);
}

void strataResultFree(StrataResult* r) {
    if (!r) return;
    delete[] r->output;
    delete[] r->diagnostics;
    r->output = nullptr;
    r->diagnostics = nullptr;
}

const char* strataLLVMVersion(void) {
#if defined(STRATA_ENABLE_LLVM)
    static char buf[32];
    unsigned maj = 0, min = 0, pat = 0;
    LLVMGetVersion(&maj, &min, &pat);
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);
    return buf;
#else
    return "0.0.0";
#endif
}

} // extern "C"
