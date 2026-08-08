#include "strata/strata.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void PrintHelp(void)
{
    unsigned capabilities = strataCapabilities();
    fprintf(stderr, "stratac - Strata compiler\n"
                    "Usage: stratac [options] <file.strata>\n"
                    "Emits a relocatable object file (.o) by default.\n"
                    "Options:\n"
                    "  -o <file>        output object file (default: <input>.o)\n"
                    "  --asm            also emit assembly (<output>.s)\n"
                    "  --ast            also print the AST to stderr\n"
                    "  --arch           set the output architecture (default: auto, x64, arm64)\n"
                    "  --no-simd        disable output of SIMD intrinsics or instructions\n"
                    "  --emit-c         emit portable C source instead of an object\n"
                    "  --run            JIT and run an int(void) entry in memory\n"
                    "  --entry <name>   entry for --run (default: main)\n"
                    "  --version        print version and exit\n"
                    "  -h, --help       show this help\n");
    if (!(capabilities & STRATA_CAP_LLVM_AOT))
    {
        fprintf(stderr, "This build has no LLVM object/assembly backend.\n");
    }
    if (!(capabilities & STRATA_CAP_TCC_JIT))
    {
        fprintf(stderr, "This build has no in-memory TinyCC backend.\n");
    }
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
    bool run = false;
    const char* entryName = "main";

    StrataArch outputArch = STRATA_ARCH_AUTO;
    StrataEmitFlags emitFlags = 0;

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
            unsigned capabilities = strataCapabilities();
            printf("stratac 0.1.0 (C%s%s", capabilities & STRATA_CAP_TCC_JIT ? ", TinyCC JIT" : "",
                   capabilities & STRATA_CAP_LLVM_AOT ? ", LLVM " : "");
            if (capabilities & STRATA_CAP_LLVM_AOT)
            {
                printf("%s", strataLLVMVersion());
            }
            printf(")\n");
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

        else if (strcmp(a, "--arch") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --arch needs an argument (auto, x64, arm64)\n");
                return 2;
            }

            const char* value = argv[++i];

            if (strcmp(value, "x64") == 0)
            {
                outputArch = STRATA_ARCH_X64;
            }
            else if (strcmp(value, "arm64") == 0)
            {
                outputArch = STRATA_ARCH_ARM64;
            }
            else if (strcmp(value, "auto") == 0)
            {
                outputArch = STRATA_ARCH_AUTO;
            }
            else
            {
                fprintf(stderr, "error: unknown architecture\n");
                return 2;
            }
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
        else if (strcmp(a, "--no-simd") == 0)
        {
            emitFlags |= STRATA_EMIT_NO_SIMD;
        }
        else if (strcmp(a, "--run") == 0)
        {
            run = true;
        }
        else if (strcmp(a, "--entry") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --entry needs an argument\n");
                return 2;
            }
            entryName = argv[++i];
        }
        else if (a[0] == '-' && a[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            PrintHelp();
            return 2;
        }
        else
        {
            if (inputFile)
            {
                fprintf(stderr, "error: multiple input files are not supported\n");
                return 2;
            }
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
    strataSetArchitecture(compiler, outputArch);

    if (!run && strcmp(entryName, "main") != 0)
    {
        fprintf(stderr, "error: --entry requires --run\n");
        strataCompilerDestroy(compiler);
        return 2;
    }

    if (run && (emitC || emitAsm || outFile))
    {
        fprintf(stderr, "error: --run cannot be combined with --emit-c, --asm, or -o\n");
        strataCompilerDestroy(compiler);
        return 2;
    }

    if (printAst)
    {
        StrataResult r = strataCompileFile(compiler, inputFile, STRATA_EMIT_AST, 0);
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

    if (run)
    {
        if (!(strataCapabilities() & STRATA_CAP_TCC_JIT))
        {
            fprintf(stderr, "error: TinyCC JIT backend not built\n");
            strataCompilerDestroy(compiler);
            return 1;
        }

        const char* error = NULL;
        StrataJit* jit = strataJitCompileFile(compiler, inputFile, &error);
        if (!jit)
        {
            fprintf(stderr, "%s\n", error ? error : "JIT compilation failed");
            strataFree((char*)error);
            strataCompilerDestroy(compiler);
            return 1;
        }

        size_t externCount = strataJitGetExternSymbolCount(jit);
        if (externCount)
        {
            fprintf(stderr, "error: --run cannot resolve host externs:");
            for (size_t i = 0; i < externCount; ++i)
            {
                fprintf(stderr, "%s%s", i ? ", " : " ", strataJitGetExternSymbolName(jit, i));
            }
            fprintf(stderr, "\n");
            strataJitDestroy(jit);
            strataCompilerDestroy(compiler);
            return 1;
        }

        if (!strataJitCanInvokeIntVoid(jit, entryName))
        {
            fprintf(stderr, "error: entry '%s' must be a defined int(void) function\n", entryName);
            strataJitDestroy(jit);
            strataCompilerDestroy(compiler);
            return 1;
        }

        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, entryName);
        if (!entry)
        {
            fprintf(stderr, "error: entry '%s' was not found\n", entryName);
            strataJitDestroy(jit);
            strataCompilerDestroy(compiler);
            return 1;
        }

        int exitCode = entry();
        strataJitDestroy(jit);
        strataCompilerDestroy(compiler);
        return exitCode;
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

        StrataResult result = strataCompileFile(compiler, inputFile, STRATA_EMIT_C, emitFlags);
        if (result.diagnostics && result.diagnostics[0])
        {
            fprintf(stderr, "%s\n", result.diagnostics);
        }
        if (!result.ok)
        {
            strataResultFree(&result);
            strataCompilerDestroy(compiler);
            if (outFileOwned)
            {
                free((void*)outFile);
            }
            return 1;
        }

        FILE* output = fopen(outFile, "wb");
        if (!output)
        {
            fprintf(stderr, "error: cannot open output '%s'\n", outFile);
            strataResultFree(&result);
            strataCompilerDestroy(compiler);
            if (outFileOwned)
            {
                free((void*)outFile);
            }
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
            if (outFileOwned)
            {
                free((void*)outFile);
            }
            return 1;
        }
        fprintf(stderr, "wrote C source: %s\n", outFile);
        strataCompilerDestroy(compiler);
        if (outFileOwned)
        {
            free((void*)outFile);
        }
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
