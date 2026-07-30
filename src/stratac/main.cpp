// stratac: the Strata compiler driver.
//
// Usage:
//   stratac [options] <file.strata>
//
// Options:
//   --emit {ir|ast}   output kind (default: ir)
//   -o <file>         write output to <file> (default: stdout)
//   --no-llvm         use the text IR back-end instead of the in-process LLVM back-end
//   --version         print version and linked LLVM version, then exit
//   -h, --help        show this help
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"

#if defined(STRATA_ENABLE_LLVM)
#include "strata/Codegen/LLVMCApi.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void printHelp() {
    std::fprintf(stderr,
        "stratac - Strata compiler\n"
        "Usage: stratac [options] <file.strata>\n"
        "Options:\n"
        "  --emit {ir|ast}  output kind (default: ir)\n"
        "  -o <file>        write output to <file> (default: stdout)\n"
        "  --no-llvm        use the text IR back-end\n"
        "  --version        print version and exit\n"
        "  -h, --help       show this help\n");
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string emit = "ir";
    std::string outFile;
    std::string inputFile;
    bool useLLVM = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { printHelp(); return 0; }
        if (a == "--version") {
#if defined(STRATA_ENABLE_LLVM)
            unsigned maj = 0, min = 0, pat = 0;
            LLVMGetVersion(&maj, &min, &pat);
            std::printf("stratac 0.1.0 (LLVM %u.%u.%u)\n", maj, min, pat);
#else
            std::printf("stratac 0.1.0 (no LLVM linkage)\n");
#endif
            return 0;
        }
        if (a == "--emit") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --emit needs an argument\n"); return 2; }
            emit = argv[++i];
            if (emit != "ir" && emit != "ast") {
                std::fprintf(stderr, "error: --emit must be 'ir' or 'ast'\n"); return 2;
            }
        } else if (a == "-o") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: -o needs an argument\n"); return 2; }
            outFile = argv[++i];
        } else if (a == "--no-llvm") {
            useLLVM = false;
        } else if (a == "--llvm") {
            useLLVM = true;
        } else if (a.size() > 0 && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            printHelp();
            return 2;
        } else {
            inputFile = a;
        }
    }

    if (inputFile.empty()) {
        std::fprintf(stderr, "error: no input file\n");
        printHelp();
        return 2;
    }

    bool ok = false;
    std::string source = readFile(inputFile, ok);
    if (!ok) {
        std::fprintf(stderr, "error: cannot open file '%s'\n", inputFile.c_str());
        return 1;
    }

    strata::SourceManager src;
    src.setSource(std::move(source), inputFile);

    strata::DiagnosticEngine diag;
    strata::Lexer lex(src.source(), diag);
    strata::Parser parser(lex, diag, inputFile);
    auto mod = parser.parseModule();

    if (diag.count() > 0) {
        std::string d = diag.format(src);
        std::fwrite(d.data(), 1, d.size(), stderr);
    }

    if (diag.hasErrors()) {
        std::fprintf(stderr, "%u error(s).\n", diag.errorCount());
        return 1;
    }

    std::string output;
    if (emit == "ast") {
        output = strata::dumpAST(*mod);
    } else {
        std::unique_ptr<strata::CodegenBackend> backend;
        if (useLLVM) backend = strata::createLLVMBackend();
        if (!backend) {
            if (useLLVM) {
                std::fprintf(stderr, "note: LLVM back-end unavailable, using text IR\n");
            }
            backend = strata::createTextBackend();
        }
        std::fprintf(stderr, "using back-end: %s\n", backend->name().data());
        auto res = backend->generate(*mod);
        output = res.output;
    }

    if (outFile.empty()) {
        std::fwrite(output.data(), 1, output.size(), stdout);
        if (!output.empty() && output.back() != '\n') std::fputc('\n', stdout);
    } else {
        std::ofstream of(outFile, std::ios::binary);
        if (!of) {
            std::fprintf(stderr, "error: cannot write output file '%s'\n", outFile.c_str());
            return 1;
        }
        of.write(output.data(), static_cast<std::streamsize>(output.size()));
    }

    return 0;
}
