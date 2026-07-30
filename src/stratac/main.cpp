// The Strata compiler driver.

#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#ifdef STRATA_ENABLE_LLVM
#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "strata/Codegen/LLVMCApi.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>
#include <string_view>

namespace
{

void PrintHelp()
{
    std::fprintf(stderr, "stratac - Strata compiler\n"
                         "Usage: stratac [options] <file.strata>\n"
                         "Emits a relocatable object file (.o) by default.\n"
                         "Options:\n"
                         "  -o <file>        output object file (default: <input>.o)\n"
                         "  --asm            also emit assembly (<output>.s)\n"
                         "  --ast            also print the AST to stderr\n"
                         "  --target <arch>  target: x86_64 (default), aarch64, arm64\n"
                         "  --version        print version and exit\n"
                         "  -h, --help       show this help\n");
}

std::optional<std::string> ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return {};
    }

    std::ostringstream ss;
    ss << in.rdbuf();

    return ss.str();
}

// Replaces the file extension of `path` with `ext`.
std::string ReplaceExt(const std::string& path, std::string_view ext)
{
    auto slash = path.find_last_of("/\\");
    auto dot = path.find_last_of('.');

    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        return path.substr(0, dot) + std::string(ext);
    }

    return path + std::string(ext);
}

} // namespace

int main(int argc, char** argv)
{
    std::string outFile;
    std::string inputFile;
    std::string targetArch;
    bool emitAsm = false;
    bool printAst = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            PrintHelp();
            return 0;
        }

        if (a == "--version")
        {
#ifdef STRATA_ENABLE_LLVM
            unsigned maj = 0;
            unsigned min = 0;
            unsigned pat = 0;
            LLVMGetVersion(&maj, &min, &pat);
            std::printf("stratac 0.1.0 (LLVM %u.%u.%u)\n", maj, min, pat);
#else
            std::printf("stratac 0.1.0 (no LLVM linkage)\n");
#endif
            return 0;
        }

        if (a == "-o")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: -o needs an argument\n");
                return 2;
            }

            outFile = argv[++i];
        }
        else if (a == "--asm")
        {
            emitAsm = true;
        }
        else if (a == "--ast")
        {
            printAst = true;
        }
        else if (a == "--target")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: --target needs an argument\n");
                return 2;
            }

            targetArch = argv[++i];
            if (targetArch != "x86_64" && targetArch != "aarch64" && targetArch != "arm64")
            {
                std::fprintf(stderr, "error: --target must be 'x86_64', 'aarch64', or 'arm64'\n");
                return 2;
            }

            if (targetArch == "arm64")
            {
                targetArch = "aarch64";
            }
        }
        else if (!a.empty() && a[0] == '-')
        {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            PrintHelp();
            return 2;
        }
        else
        {
            inputFile = a;
        }
    }

    if (inputFile.empty())
    {
        std::fprintf(stderr, "error: no input file\n");
        PrintHelp();
        return 2;
    }

    std::optional<std::string> source = ReadFile(inputFile);

    if (!source.has_value())
    {
        std::fprintf(stderr, "error: cannot open file '%s'\n", inputFile.c_str());
        return 1;
    }

    strata::SourceManager src;
    src.SetSource(std::move(*source), inputFile);

    strata::DiagnosticEngine diag;
    strata::Lexer lex(src.Source(), diag);
    strata::Parser parser(lex, diag, inputFile);
    auto mod = parser.ParseModule();
    strata::ResolveOverloads(*mod, diag);

    if (diag.Count() > 0)
    {
        std::string d = diag.Format(src);
        std::fwrite(d.data(), 1, d.size(), stderr);
    }

    if (diag.HasErrors())
    {
        std::fprintf(stderr, "%u error(s).\n", diag.ErrorCount());
        return 1;
    }

    if (printAst)
    {
        std::string ast = strata::DumpAst(*mod);
        std::fwrite(ast.data(), 1, ast.size(), stderr);
    }

#ifdef STRATA_ENABLE_LLVM
    std::string notes;
    strata::BuiltModule bm = strata::BuildLlvmModule(*mod, diag, notes);

    if (!notes.empty())
    {
        std::fwrite(notes.data(), 1, notes.size(), stderr);
    }

    if (diag.HasErrors())
    {
        std::string d = diag.Format(src);
        std::fwrite(d.data(), 1, d.size(), stderr);
        std::fprintf(stderr, "%u error(s).\n", diag.ErrorCount());
        return 1;
    }

    // Build the full target triple when cross-compiling.
    std::string triple;
    if (!targetArch.empty() && targetArch != "x86_64")
    {
        char* hostTriple = LLVMGetDefaultTargetTriple();
        std::string_view host(hostTriple);
        auto firstDash = host.find('-');
        if (firstDash != std::string_view::npos)
        {
            triple = targetArch + std::string(host.substr(firstDash));
        }
        else
        {
            triple = targetArch + "-pc-windows-msvc";
        }
        LLVMDisposeMessage(hostTriple);
    }

    // Derive output path from input if -o was not given.
    if (outFile.empty())
    {
        outFile = ReplaceExt(inputFile, ".o");
    }

    std::string err;

    if (!strata::EmitNativeFile(bm, outFile, false, err, triple))
    {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr, "wrote object: %s\n", outFile.c_str());

    if (emitAsm)
    {
        std::string asmFile = ReplaceExt(outFile, ".s");

        if (!strata::EmitNativeFile(bm, asmFile, true, err, triple))
        {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }

        std::fprintf(stderr, "wrote assembly: %s\n", asmFile.c_str());
    }

    return 0;
#else
    std::fprintf(stderr, "error: object emission requires LLVM linkage\n");
    return 1;
#endif
}
