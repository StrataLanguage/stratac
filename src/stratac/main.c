#include "strata/strata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void PrintHelp(void)
{
    fprintf(stderr, "stratac - Strata compiler\n"
                    "Usage: stratac [options] <file.strata>\n"
                    "Emits a relocatable object file (.o) by default.\n"
                    "Options:\n"
                    "  -o <file>        output object file (default: <input>.o)\n"
                    "  --asm            also emit assembly (<output>.s)\n"
                    "  --ast            also print the AST to stderr\n"
                    "  --emit-c         emit portable C source instead of an object\n"
                    "  --version        print version and exit\n"
                    "  -h, --help       show this help\n");
}

static char* ReplaceExt(const char* path, const char* ext)
{
    char* slash = strrchr(path, '/');
    char* bslash = strrchr(path, '\\');
    char* lastSep = bslash > slash ? bslash : slash;

    char* dot = strrchr(path, '.');
    if (dot && (!lastSep || dot > lastSep))
    {
        size_t baseLen = dot - path;
        size_t extLen = strlen(ext);
        char* result = malloc(baseLen + extLen + 1);
        memcpy(result, path, baseLen);
        memcpy(result + baseLen, ext, extLen + 1);
        return result;
    }

    size_t len = strlen(path);
    size_t extLen = strlen(ext);
    char* result = malloc(len + extLen + 1);
    memcpy(result, path, len);
    memcpy(result + len, ext, extLen + 1);
    return result;
}

int main(int argc, char** argv)
{
    const char* outFile = NULL;
    const char* inputFile = NULL;
    bool emitAsm = false;
    bool printAst = false;
    bool emitC = false;
    bool outFileOwned = false;

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
            printf("stratac 0.1.0 (%s)\n", strataLLVMVersion());
            return 0;
        }

        if (strcmp(a, "-o") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: -o needs an argument\n");
                return 2;
            }
            outFile = argv[++i];
        }
        else if (strcmp(a, "--asm") == 0)
        {
            emitAsm = true;
        }
        else if (strcmp(a, "--ast") == 0)
        {
            printAst = true;
        }
        else if (strcmp(a, "--emit-c") == 0)
        {
            emitC = true;
        }
        else if (a[0] == '-' && a[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            PrintHelp();
            return 2;
        }
        else
        {
            inputFile = a;
        }
    }

    if (!inputFile)
    {
        fprintf(stderr, "error: no input file\n");
        PrintHelp();
        return 2;
    }

    StrataCompiler* compiler = strataCompilerCreate();

    if (printAst)
    {
        StrataResult r = strataCompileFile(compiler, inputFile, STRATA_EMIT_AST);
        if (r.diagnostics && r.diagnostics[0])
        {
            fprintf(stderr, "%s\n", r.diagnostics);
        }
        if (r.ok && r.output)
        {
            fprintf(stderr, "%s\n", r.output);
        }
        strataResultFree(&r);
    }

    if (emitC)
    {
        if (emitAsm)
        {
            fprintf(stderr, "error: --asm cannot be combined with --emit-c\n");
            strataCompilerDestroy(compiler);
            return 2;
        }

        if (!outFile)
        {
            outFile = ReplaceExt(inputFile, ".c");
            outFileOwned = true;
        }

        StrataResult result = strataCompileFile(compiler, inputFile, STRATA_EMIT_C);
        if (result.diagnostics && result.diagnostics[0])
        {
            fprintf(stderr, "%s\n", result.diagnostics);
        }
        if (!result.ok)
        {
            strataResultFree(&result);
            strataCompilerDestroy(compiler);
            if (outFileOwned) free((void*)outFile);
            return 1;
        }

        FILE* output = fopen(outFile, "wb");
        if (!output)
        {
            fprintf(stderr, "error: cannot open output '%s'\n", outFile);
            strataResultFree(&result);
            strataCompilerDestroy(compiler);
            if (outFileOwned) free((void*)outFile);
            return 1;
        }
        size_t outputLen = strlen(result.output);
        bool wrote = fwrite(result.output, 1, outputLen, output) == outputLen;
        wrote = fclose(output) == 0 && wrote;
        strataResultFree(&result);
        if (!wrote)
        {
            fprintf(stderr, "error: failed writing C source '%s'\n", outFile);
            strataCompilerDestroy(compiler);
            if (outFileOwned) free((void*)outFile);
            return 1;
        }
        fprintf(stderr, "wrote C source: %s\n", outFile);
        strataCompilerDestroy(compiler);
        if (outFileOwned) free((void*)outFile);
        return 0;
    }

    if (!outFile)
    {
        outFile = ReplaceExt(inputFile, ".o");
        outFileOwned = true;
    }

    const char* err = NULL;
    int ok = strataCompileToObject(compiler, inputFile, outFile, 0, &err);

    if (!ok)
    {
        fprintf(stderr, "%s\n", err ? err : "compilation failed");
        strataFree((char*)err);
        strataCompilerDestroy(compiler);
        if (outFileOwned)
        {
            free((void*)outFile);
        }
        return 1;
    }

    fprintf(stderr, "wrote object: %s\n", outFile);

    if (emitAsm)
    {
        char* asmFile = ReplaceExt(outFile, ".s");
        const char* asmErr = NULL;
        int asmOk = strataCompileToObject(compiler, inputFile, asmFile, 1, &asmErr);

        if (!asmOk)
        {
            fprintf(stderr, "error writing assembly: %s\n", asmErr ? asmErr : "(no message)");
            strataFree((char*)asmErr);
        }
        else
        {
            fprintf(stderr, "wrote assembly: %s\n", asmFile);
        }

        free(asmFile);
    }

    if (outFileOwned)
    {
        free((void*)outFile);
    }

    strataCompilerDestroy(compiler);
    return 0;
}
