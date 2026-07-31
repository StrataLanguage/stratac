#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "strata/Codegen/LLVMCApi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void PrintHelp(void)
{
    fprintf(stderr, "stratac - Strata compiler\n"
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

static char* DupString(const char* s)
{
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (out)
    {
        memcpy(out, s, n + 1);
    }
    return out;
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

static char* ReplaceExt(const char* path, const char* ext)
{
    size_t pathLen = strlen(path);
    size_t extLen = strlen(ext);

    size_t slashPos = (size_t)-1;
    size_t dotPos = (size_t)-1;

    for (size_t i = 0; i < pathLen; i++)
    {
        char c = path[i];
        if (c == '/' || c == '\\')
        {
            slashPos = i;
        }
        else if (c == '.')
        {
            dotPos = i;
        }
    }

    size_t prefix;
    if (dotPos != (size_t)-1 && (slashPos == (size_t)-1 || dotPos > slashPos))
    {
        prefix = dotPos;
    }
    else
    {
        prefix = pathLen;
    }

    char* result = (char*)malloc(prefix + extLen + 1);
    if (!result)
    {
        return NULL;
    }

    memcpy(result, path, prefix);
    memcpy(result + prefix, ext, extLen);
    result[prefix + extLen] = '\0';
    return result;
}

int main(int argc, char** argv)
{
    char* outFile = NULL;
    char* inputFile = NULL;
    char* targetArch = NULL;
    bool emitAsm = false;
    bool printAst = false;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
        {
            PrintHelp();
            return 0;
        }

        if (strcmp(a, "--version") == 0)
        {
            unsigned maj = 0;
            unsigned min = 0;
            unsigned pat = 0;
            LLVMGetVersion(&maj, &min, &pat);
            printf("stratac 0.1.0 (LLVM %u.%u.%u)\n", maj, min, pat);
            return 0;
        }

        if (strcmp(a, "-o") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: -o needs an argument\n");
                return 2;
            }
            free(outFile);
            outFile = DupString(argv[++i]);
        }
        else if (strcmp(a, "--asm") == 0)
        {
            emitAsm = true;
        }
        else if (strcmp(a, "--ast") == 0)
        {
            printAst = true;
        }
        else if (strcmp(a, "--target") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --target needs an argument\n");
                return 2;
            }
            free(targetArch);
            targetArch = DupString(argv[++i]);
            if (strcmp(targetArch, "x86_64") != 0 &&
                strcmp(targetArch, "aarch64") != 0 &&
                strcmp(targetArch, "arm64") != 0)
            {
                fprintf(stderr, "error: --target must be 'x86_64', 'aarch64', or 'arm64'\n");
                return 2;
            }
            if (strcmp(targetArch, "arm64") == 0)
            {
                free(targetArch);
                targetArch = DupString("aarch64");
            }
        }
        else if (a[0] != '\0' && a[0] == '-')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            PrintHelp();
            return 2;
        }
        else
        {
            free(inputFile);
            inputFile = DupString(a);
        }
    }

    if (!inputFile)
    {
        fprintf(stderr, "error: no input file\n");
        PrintHelp();
        return 2;
    }

    size_t sourceLen = 0;
    char* source = ReadFile(inputFile, &sourceLen);

    if (!source)
    {
        fprintf(stderr, "error: cannot open file '%s'\n", inputFile);
        return 1;
    }

    Arena arena;
    arena_init(&arena, 0);

    SourceManager src;
    SourceManagerInit(&src);
    SourceManagerSetSource(&src, source, sourceLen, inputFile);

    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    Lexer lex;
    LexerInit(&lex, src.m_text, src.m_textLen, &diag);

    Parser parser;
    ParserInit(&parser, &lex, &diag, &arena, inputFile);

    Module* mod = ParserParseModule(&parser);
    ResolveOverloads(mod, &diag, &arena);

    if (DiagCount(&diag) > 0)
    {
        char* d = DiagFormat(&diag, &src, &arena);
        fwrite(d, 1, strlen(d), stderr);
    }

    if (DiagHasErrors(&diag))
    {
        fprintf(stderr, "%u error(s).\n", DiagErrorCount(&diag));
        return 1;
    }

    if (printAst)
    {
        char* ast = DumpAst(mod, &arena);
        fwrite(ast, 1, strlen(ast), stderr);
    }

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false);

    if (DiagHasErrors(&diag))
    {
        if (DiagCount(&diag) > 0)
        {
            char* d = DiagFormat(&diag, &src, &arena);
            fwrite(d, 1, strlen(d), stderr);
        }
        fprintf(stderr, "%u error(s).\n", DiagErrorCount(&diag));
        return 1;
    }

    char* triple = NULL;
    if (targetArch && strcmp(targetArch, "x86_64") != 0)
    {
        char* hostTriple = LLVMGetDefaultTargetTriple();
        const char* firstDash = strchr(hostTriple, '-');

        const char* suffix;
        size_t suffixLen;

        if (firstDash)
        {
            suffix = firstDash;
            suffixLen = strlen(firstDash);
        }
        else
        {
            suffix = "-pc-windows-msvc";
            suffixLen = strlen(suffix);
        }

        size_t tLen = strlen(targetArch);
        triple = (char*)malloc(tLen + suffixLen + 1);
        if (triple)
        {
            memcpy(triple, targetArch, tLen);
            memcpy(triple + tLen, suffix, suffixLen);
            triple[tLen + suffixLen] = '\0';
        }

        LLVMDisposeMessage(hostTriple);
    }

    if (!outFile)
    {
        outFile = ReplaceExt(inputFile, ".o");
    }

    char* err = NULL;

    if (!EmitNativeFile(&bm, outFile, false, &err, triple))
    {
        fprintf(stderr, "error: %s\n", err ? err : "(unknown)");
        return 1;
    }

    fprintf(stderr, "wrote object: %s\n", outFile);

    if (emitAsm)
    {
        char* asmFile = ReplaceExt(outFile, ".s");

        if (!EmitNativeFile(&bm, asmFile, true, &err, triple))
        {
            fprintf(stderr, "error: %s\n", err ? err : "(unknown)");
            free(asmFile);
            return 1;
        }

        fprintf(stderr, "wrote assembly: %s\n", asmFile);
        free(asmFile);
    }

    BuiltModuleDispose(&bm);
    return 0;
}
