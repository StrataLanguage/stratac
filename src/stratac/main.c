#include "strata/Codegen/CodegenBackend.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "strata/Codegen/LLVMCApi.h"

#include <Defines.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum ResultCode
{
    RCSuccess,
    RCErrorFileIO = 1,
    RCErrorInvalidArgument,
};

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

typedef struct
{
    const char* name;
    const char* longName;
    const char* archCode;
    const char* fallbackSuffix;
} StrataPlatform;

static const StrataPlatform registeredPlatforms[] = {
    (StrataPlatform){"win", "windows", "x64_64", "-pc-windows-msvc"},
    (StrataPlatform){"mac", "macos", "arm64", "-apple-darwin-25.0.0"},
};

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

/**
 * @brief Finds the platform that matches the requested name `request`. Returns NULL if no platform was found.
 */
static const StrataPlatform* GetPlatform(const char* request)
{
    const int platformCount = ARRAY_COUNT(registeredPlatforms);

    for (int i = 0; i < platformCount; i++)
    {
        const StrataPlatform* platform = &registeredPlatforms[i];
        if (!strcmp(request, platform->name) || !strcmp(request, platform->longName))
        {
            return platform;
        }
    }

    return NULL;
}

char* BuildTargetTriple(const char* requestedPlatform)
{
    char* resultTriple = NULL;

    if (requestedPlatform == NULL)
    {
        return NULL;
    }

    const char* targetSuffix = "";

    const StrataPlatform* platform = GetPlatform(requestedPlatform);

    char* hostTriple = LLVMGetDefaultTargetTriple();
    const char* firstDash = strchr(hostTriple, '-');

    const char* suffix;

    if (firstDash)
    {
        suffix = firstDash;
    }
    else
    {
        suffix = platform->fallbackSuffix;
    }

    const size_t suffixLen = strlen(suffix);

    // Build the final triple
    {
        size_t archCodeLen = strlen(platform->archCode);
        resultTriple = (char*)malloc(archCodeLen + suffixLen + 1);

        if (resultTriple)
        {
            memcpy(resultTriple, platform->archCode, archCodeLen);
            memcpy(resultTriple + archCodeLen, suffix, suffixLen);
            resultTriple[archCodeLen + suffixLen] = '\0';
        }
    }

    LLVMDisposeMessage(hostTriple);

    return resultTriple;
}

int main(int argc, char** argv)
{
    char* outFile = NULL;
    char* inputFile = NULL;
    char* requestedPlatform = NULL;
    bool emitAsm = false;
    bool printAst = false;

    for (int argIndex = 1; argIndex < argc; argIndex++)
    {
        const char* arg = argv[argIndex];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            PrintHelp();
            return RCSuccess;
        }

        if (strcmp(arg, "--version") == 0)
        {
            unsigned maj = 0;
            unsigned min = 0;
            unsigned pat = 0;
            LLVMGetVersion(&maj, &min, &pat);
            printf("stratac 0.1.0 (LLVM %u.%u.%u)\n", maj, min, pat);
            return RCSuccess;
        }

        if (strcmp(arg, "-o") == 0)
        {
            if (argIndex + 1 >= argc)
            {
                fprintf(stderr, "error: -o needs an argument\n");
                return RCErrorInvalidArgument;
            }
            free(outFile);
            outFile = DupString(argv[++argIndex]);
        }
        else if (strcmp(arg, "--asm") == 0)
        {
            emitAsm = true;
        }
        else if (strcmp(arg, "--ast") == 0)
        {
            printAst = true;
        }
        else if (strcmp(arg, "--target") == 0)
        {
            if (argIndex + 1 >= argc)
            {
                fprintf(stderr, "error: --target needs an argument\n");
                return RCErrorInvalidArgument;
            }
            free(requestedPlatform);
            requestedPlatform = DupString(argv[++argIndex]);

            const StrataPlatform* platform = GetPlatform(requestedPlatform);

            if (platform == NULL)
            {
                fprintf(stderr, "error: --target must be one of the following:\n");
                for (int i = 0; i < ARRAY_COUNT(registeredPlatforms); i++)
                {
                    const StrataPlatform* currentPlatform = &registeredPlatforms[i];
                    fprintf(stderr, "\t%s|%s\n", currentPlatform->name, currentPlatform->longName);
                }

                return RCErrorInvalidArgument;
            }
        }
        else if (arg[0] != '\0' && arg[0] == '-')
        {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            PrintHelp();
            return RCErrorInvalidArgument;
        }
        else
        {
            free(inputFile);
            inputFile = DupString(arg);
        }
    }

    if (!inputFile)
    {
        fprintf(stderr, "error: no input file\n");
        PrintHelp();
        return RCErrorInvalidArgument;
    }

    size_t sourceLen = 0;
    char* source = ReadFile(inputFile, &sourceLen);

    if (!source)
    {
        fprintf(stderr, "error: cannot open file '%s'\n", inputFile);
        return RCErrorFileIO;
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
        return RCErrorFileIO;
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
        return RCErrorFileIO;
    }

    char* triple = BuildTargetTriple(requestedPlatform);

    if (!outFile)
    {
        outFile = ReplaceExt(inputFile, ".o");
    }

    char* err = NULL;

    if (!EmitNativeFile(&bm, outFile, false, &err, triple))
    {
        fprintf(stderr, "error: %s\n", err ? err : "(unknown)");
        return RCErrorFileIO;
    }

    fprintf(stderr, "wrote object: %s\n", outFile);

    if (emitAsm)
    {
        char* asmFile = ReplaceExt(outFile, ".s");

        if (!EmitNativeFile(&bm, asmFile, true, &err, triple))
        {
            fprintf(stderr, "error: %s\n", err ? err : "(unknown)");
            free(asmFile);
            return RCErrorFileIO;
        }

        fprintf(stderr, "wrote assembly: %s\n", asmFile);
        free(asmFile);
    }

    BuiltModuleDispose(&bm);
    return RCSuccess;
}
