// Strata compiler: implementation of the public C embedding API (strata.h).
#include "strata/strata.h"

#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>

#ifdef STRATA_ENABLE_LLVM
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "strata/Codegen/LLVMCApi.h"
#endif

struct StrataCompiler
{
    bool useLLVM = true;
};

extern "C"
{
    StrataCompiler* strataCompilerCreate(void)
    {
        return new StrataCompiler;
    }

    void strataCompilerDestroy(StrataCompiler* c)
    {
        delete c;
    }

    void strataCompilerUseLLVM(StrataCompiler* c, int enabled)
    {
        if (c)
        {
            c->useLLVM = enabled != 0;
        }
    }

    static char* DupCString(const std::string& s)
    {
        char* p = new (std::nothrow) char[s.size() + 1];
        if (!p)
        {
            return nullptr;
        }

        std::memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }

    static StrataResult CompileSource(StrataCompiler* c, std::string source, std::string moduleName,
                                      StrataEmitKind emit)
    {
        StrataResult r{};
        r.ok = 0;
        r.output = DupCString("");
        r.diagnostics = DupCString("");

        strata::SourceManager src;
        src.SetSource(std::move(source), moduleName);

        strata::DiagnosticEngine diag;
        strata::Lexer lex(src.Source(), diag);
        strata::Parser parser(lex, diag, moduleName);
        
        auto mod = parser.ParseModule();

        strata::ResolveOverloads(*mod, diag);

        std::string diagText = diag.Format(src);
        std::string out;

        if (!diag.HasErrors() && mod)
        {
            if (emit == STRATA_EMIT_AST)
            {
                out = strata::DumpAst(*mod);
            }
            else
            {
                std::unique_ptr<strata::CodegenBackend> backend;
                if (c && c->useLLVM)
                {
                    backend = strata::CreateLlvmBackend();
                }

                if (!backend)
                {
                    backend = strata::CreateTextBackend();
                }

                auto result = backend->Generate(*mod);
                out = result.output;
                if (!result.ok)
                {
                    diag.Error({}, "code generation failed");
                }
            }
        }

        delete[] r.output;
        delete[] r.diagnostics;
        r.output = DupCString(out);
        r.diagnostics = DupCString(diagText);
        r.error_count = diag.ErrorCount();
        r.ok = (!diag.HasErrors()) ? 1 : 0;
        return r;
    }

    StrataResult strataCompileString(StrataCompiler* c, const char* source, const char* moduleName, StrataEmitKind emit)
    {
        if (!c || !source)
        {
            StrataResult r{};
            r.ok = 0;
            r.output = DupCString("");
            r.diagnostics = DupCString("null compiler or source");
            return r;
        }

        return CompileSource(c, std::string(source),
                             moduleName ? std::string(moduleName) : std::string("strata_module"), emit);
    }

    StrataResult strataCompileFile(StrataCompiler* c, const char* path, StrataEmitKind emit)
    {
        if (!c || !path)
        {
            StrataResult r{};
            r.ok = 0;
            r.output = DupCString("");
            r.diagnostics = DupCString("null compiler or path");
            return r;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            StrataResult r{};
            r.ok = 0;
            r.output = DupCString("");
            std::string m = std::string("cannot open file: ") + path;
            r.diagnostics = DupCString(m);
            r.error_count = 1;
            return r;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        std::string base = path;
        // Use the file name (without path) as the module name.
        auto slash = base.find_last_of("/\\");
        std::string moduleName = (slash != std::string::npos) ? base.substr(slash + 1) : base;
        return CompileSource(c, ss.str(), moduleName, emit);
    }

    void strataResultFree(StrataResult* r)
    {
        if (!r)
        {
            return;
        }

        delete[] r->output;
        delete[] r->diagnostics;
        r->output = nullptr;
        r->diagnostics = nullptr;
    }

    const char* strataLLVMVersion(void)
    {
#ifdef STRATA_ENABLE_LLVM
        static char buf[32];
        unsigned maj = 0;
        unsigned min = 0;
        unsigned pat = 0;
        LLVMGetVersion(&maj, &min, &pat);
        std::snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);
        return buf;
#else
        return "0.0.0";
#endif
    }

    // ---------------------------------------------------------------------------
    // JIT execution
    // ---------------------------------------------------------------------------
    struct StrataJit
    {
#ifdef STRATA_ENABLE_LLVM
        std::unique_ptr<strata::LLVMJit> jit;
#endif
        std::string diagnostics;
    };

#ifdef STRATA_ENABLE_LLVM
    static StrataJit* JitCompile(StrataCompiler* /*c*/, std::string source, std::string moduleName, const char** errOut)
    {
        strata::SourceManager src;
        src.SetSource(std::move(source), moduleName);
        strata::DiagnosticEngine diag;
        strata::Lexer lex(src.Source(), diag);
        strata::Parser parser(lex, diag, moduleName);
        auto mod = parser.ParseModule();
        strata::ResolveOverloads(*mod, diag);
        std::string diagText = diag.Format(src);

        if (diag.HasErrors() || !mod)
        {
            if (errOut)
            {
                *errOut = DupCString("parse errors:\n" + diagText);
            }

            return nullptr;
        }

        std::string notes;
        strata::BuiltModule bm = strata::BuildLlvmModule(*mod, diag, notes, /*jitMode=*/true);

        if (diag.HasErrors())
        {
            if (errOut)
            {
                std::string allDiag = diag.Format(src);
                *errOut = DupCString("codegen errors:\n" + allDiag);
            }

            return nullptr;
        }

        auto jit = std::make_unique<strata::LLVMJit>();
        std::string err;
        if (!jit->Load(std::move(bm), err))
        {
            if (errOut)
            {
                *errOut = DupCString("JIT error: " + err);
            }

            return nullptr;
        }

        auto* handle = new StrataJit{};
        handle->jit = std::move(jit);
        handle->diagnostics = diagText + notes;
        return handle;
    }

#endif

    StrataJit* strataJitCompileString(StrataCompiler* c, const char* source, const char* moduleName,
                                      const char** errOut)
    {
#ifdef STRATA_ENABLE_LLVM
        if (errOut)
        {
            *errOut = nullptr;
        }

        if (!c || !source)
        {
            if (errOut)
            {
                *errOut = DupCString("null compiler or source");
            }

            return nullptr;
        }

        return JitCompile(c, std::string(source), moduleName ? std::string(moduleName) : std::string("strata_module"),
                          errOut);
#else
        if (errOut)
        {
            *errOut = DupCString("JIT unavailable: strata built without LLVM");
        }

        return nullptr;
#endif
    }

    StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut)
    {
#ifdef STRATA_ENABLE_LLVM
        if (errOut)
        {
            *errOut = nullptr;
        }

        if (!c || !path)
        {
            if (errOut)
            {
                *errOut = DupCString("null compiler or path");
            }

            return nullptr;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            if (errOut)
            {
                *errOut = DupCString(std::string("cannot open file: ") + path);
            }

            return nullptr;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        std::string base = path;
        auto slash = base.find_last_of("/\\");
        std::string moduleName = (slash != std::string::npos) ? base.substr(slash + 1) : base;
        return JitCompile(c, ss.str(), moduleName, errOut);
#else
        if (errOut)
        {
            *errOut = DupCString("JIT unavailable: strata built without LLVM");
        }

        return nullptr;
#endif
    }

    void* strataJitGetFunction(StrataJit* jit, const char* name)
    {
#ifdef STRATA_ENABLE_LLVM
        if (!jit || !jit->jit || !name)
        {
            return nullptr;
        }

        std::uint64_t addr = jit->jit->GetAddress(name);
        return addr ? reinterpret_cast<void*>(addr) : nullptr;
#else
        (void)jit;
        (void)name;
        return nullptr;
#endif
    }

    int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn)
    {
#ifdef STRATA_ENABLE_LLVM
        if (!jit || !jit->jit || !name || !fn)
        {
            return 0;
        }

        return jit->jit->AddSymbol(name, fn) ? 1 : 0;
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

        return jit->jit->ExternSymbols().size();
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
            return nullptr;
        }

        const auto& v = jit->jit->ExternSymbols();
        if (index >= v.size())
        {
            return nullptr;
        }

        return v[index].c_str();
#else
        (void)jit;
        (void)index;
        return nullptr;
#endif
    }

    const char* strataJitDiagnostics(StrataJit* jit)
    {
        if (!jit)
        {
            return "";
        }

        return jit->diagnostics.c_str();
    }

    void strataJitDestroy(StrataJit* jit)
    {
        delete jit;
    }

    void strataFree(char* s)
    {
        delete[] s;
    }

} // extern "C"
