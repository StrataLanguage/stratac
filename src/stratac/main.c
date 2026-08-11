#include "strata/strata.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum ResultCode
{
    RCSuccess = 0,
    RCIOError = 1,
    RCArgumentError = 2,
} ResultCode;

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
                    "  --emit-ir        emit LLVM IR instead of an object\n"
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

typedef struct State
{
    const char* outFileName;
    const char* inFileName;

    bool emitAsm;
    bool printAst;
    bool emitC;
    bool emitLlvmIr;
    bool run;
    bool outFileOwned;
    const char* entryName;
    StrataArch outputArch;
    StrataEmitFlags emitFlags;
} State;

void StateDefault(State* c)
{
    memset(c, 0, sizeof(State));

    c->outFileName = NULL;
    c->inFileName = NULL;

    c->emitAsm = false;
    c->printAst = false;
    c->emitC = false;
    c->emitLlvmIr = false;
    c->run = false;
    c->outFileOwned = false;
    c->entryName = "main";

    c->outputArch = STRATA_ARCH_AUTO;
    c->emitFlags = 0;
}

static ResultCode EmitC(State* state, StrataCompiler* compiler)
{
    if (state->emitAsm)
    {
        fprintf(stderr, "error: --asm cannot be combined with --emit-c\n");
        return RCArgumentError;
    }

    if (!state->outFileName)
    {
        state->outFileName = ReplaceExt(state->inFileName, ".c");
        state->outFileOwned = true;
    }

    StrataResult result = strataCompileFile(compiler, state->inFileName, STRATA_EMIT_C, state->emitFlags);
    if (result.diagnostics && result.diagnostics[0])
    {
        fprintf(stderr, "%s\n", result.diagnostics);
    }
    if (!result.ok)
    {
        strataResultFree(&result);

        if (state->outFileOwned)
        {
            free((void*)state->outFileName);
        }

        return RCIOError;
    }

    FILE* output = fopen(state->outFileName, "wb");

    if (output == NULL)
    {
        fprintf(stderr, "error: cannot open output '%s'\n", state->outFileName);
        strataResultFree(&result);

        if (state->outFileOwned)
        {
            free((void*)state->outFileName);
        }

        return RCIOError;
    }

    size_t outputLen = strlen(result.output);
    bool wrote = fwrite(result.output, 1, outputLen, output) == outputLen;

    strataResultFree(&result);

    if (!wrote || fclose(output) != 0)
    {
        fprintf(stderr, "error: failed writing C source '%s'\n", state->outFileName);

        if (state->outFileOwned)
        {
            free((void*)state->outFileName);
        }

        return RCIOError;
    }

    fprintf(stderr, "wrote C source: %s\n", state->outFileName);

    if (state->outFileOwned)
    {
        free((void*)state->outFileName);
    }

    return RCSuccess;
}

int main(int argc, char** argv)
{
    State state;
    StateDefault(&state);

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

            state.outFileName = argv[++i];
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
                state.outputArch = STRATA_ARCH_X64;
            }
            else if (strcmp(value, "arm64") == 0)
            {
                state.outputArch = STRATA_ARCH_ARM64;
            }
            else if (strcmp(value, "auto") == 0)
            {
                state.outputArch = STRATA_ARCH_AUTO;
            }
            else
            {
                fprintf(stderr, "error: unknown architecture\n");
                return 2;
            }
        }
        else if (strcmp(a, "--asm") == 0)
        {
            state.emitAsm = true;
        }
        else if (strcmp(a, "--ast") == 0)
        {
            state.printAst = true;
        }
        else if (strcmp(a, "--emit-c") == 0)
        {
            state.emitC = true;
        }
        else if (strcmp(a, "--emit-ir") == 0)
        {
            state.emitLlvmIr = true;
        }
        else if (strcmp(a, "--no-simd") == 0)
        {
            state.emitFlags |= STRATA_EMIT_NO_SIMD;
        }
        else if (strcmp(a, "--run") == 0)
        {
            state.run = true;
        }
        else if (strcmp(a, "--entry") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --entry needs an argument\n");
                return 2;
            }
            state.entryName = argv[++i];
        }
        else if (a[0] == '-' && a[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            PrintHelp();
            return 2;
        }
        else
        {
            if (state.inFileName)
            {
                fprintf(stderr, "error: multiple input files are not supported\n");
                return 2;
            }
            state.inFileName = a;
        }
    }

    if (!state.inFileName)
    {
        fprintf(stderr, "error: no input file\n");
        PrintHelp();
        return 2;
    }

    StrataCompiler* compiler = strataCompilerCreate();
    strataSetArchitecture(compiler, state.outputArch);

    if (!state.run && strcmp(state.entryName, "main") != 0)
    {
        fprintf(stderr, "error: --entry requires --run\n");
        strataCompilerDestroy(compiler);
        return 2;
    }

    if (state.run && (state.emitC || state.emitAsm || state.outFileName))
    {
        fprintf(stderr, "error: --run cannot be combined with --emit-c, --asm, or -o\n");
        strataCompilerDestroy(compiler);
        return 2;
    }

    if (state.printAst)
    {
        StrataResult r = strataCompileFile(compiler, state.inFileName, STRATA_EMIT_AST, 0);
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

    if (state.run)
    {
        if (!(strataCapabilities() & STRATA_CAP_TCC_JIT))
        {
            fprintf(stderr, "error: TinyCC JIT backend not built\n");
            strataCompilerDestroy(compiler);
            return 1;
        }

        const char* error = NULL;
        StrataJit* jit = strataJitCompileFile(compiler, state.inFileName, &error);
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

        if (!strataJitCanInvokeIntVoid(jit, state.entryName))
        {
            fprintf(stderr, "error: entry '%s' must be a defined int(void) function\n", state.entryName);
            strataJitDestroy(jit);
            strataCompilerDestroy(compiler);
            return 1;
        }

        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, state.entryName);
        if (!entry)
        {
            fprintf(stderr, "error: entry '%s' was not found\n", state.entryName);
            strataJitDestroy(jit);
            strataCompilerDestroy(compiler);
            return 1;
        }

        int exitCode = entry();
        strataJitDestroy(jit);
        strataCompilerDestroy(compiler);
        return exitCode;
    }

    if (state.emitLlvmIr)
    {
    }

    if (state.emitC)
    {
        ResultCode result = EmitC(&state, compiler);
        strataCompilerDestroy(compiler);

        return result;
    }

    if (!state.outFileName)
    {
        state.outFileName = ReplaceExt(state.inFileName, ".o");
        state.outFileOwned = true;
    }

    const char* err = NULL;
    int ok = strataCompileToObject(compiler, state.inFileName, state.outFileName, 0, &err);

    if (!ok)
    {
        fprintf(stderr, "%s\n", err ? err : "compilation failed");
        strataFree((char*)err);
        strataCompilerDestroy(compiler);
        if (state.outFileOwned)
        {
            free((void*)state.outFileName);
        }
        return 1;
    }

    fprintf(stderr, "wrote object: %s\n", state.outFileName);

    if (state.emitAsm)
    {
        char* asmFile = ReplaceExt(state.outFileName, ".s");
        const char* asmErr = NULL;
        int asmOk = strataCompileToObject(compiler, state.inFileName, asmFile, 1, &asmErr);

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

    if (state.outFileOwned)
    {
        free((void*)state.outFileName);
    }

    strataCompilerDestroy(compiler);
    return 0;
}
