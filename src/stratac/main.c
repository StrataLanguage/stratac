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

typedef struct State State;

typedef enum CommandFlags
{
    CF_NONE = 0,
    /* The program should exit after this command */
    CF_FINAL = (1 << 0),
} CommandFlags;

typedef struct CLICommand
{
    /* The short representation of the command, e.g. `-v` */
    const char* shortCmd;
    /* The long version of the command, e.g. `--version` */
    const char* longCmd;
    /* Info that follows after the command text */
    const char* follower;
    /* The implementation */
    ResultCode (*func)(State*, StrataCompiler*);
    /* */
    CommandFlags flags;

    const char* description;
} CLICommand;

/*
 * Command Definitions
 */

static ResultCode CmdVersion(State* state, StrataCompiler* compiler);
static ResultCode CmdHelp(State* state, StrataCompiler* compiler);
static ResultCode CmdOutputFileName(State* state, StrataCompiler* compiler);
static ResultCode CmdEmitAsm(State* state, StrataCompiler* compiler);
static ResultCode CmdPrintAst(State* state, StrataCompiler* compiler);

static const CLICommand commands[] = {
    {"-o", NULL,        "<file>", &CmdOutputFileName, CF_NONE,  "output object file (default: <input>.o)"},
    {"-S", "--asm",     NULL,     &CmdEmitAsm,        CF_FINAL, "output asm representation"              },
    {NULL, "--ast",     NULL,     &CmdPrintAst,       CF_FINAL, "print ast tree"                         },

    /* Info commands */
    {"-v", "--version", NULL,     &CmdVersion,        CF_FINAL, "print version and exit"                 },
    {"-h", "--help",    NULL,     &CmdHelp,           CF_FINAL, "show available commands and usage"      },
};

const CLICommand* FindCommand(const char* req, const CLICommand* cmds, int count)
{
    for (int i = 0; i < count; i++)
    {
        const CLICommand* cmd = &cmds[i];
        if (cmd->shortCmd != NULL && strcmp(req, cmd->shortCmd) == 0)
        {
            return cmd;
        }

        if (cmd->longCmd != NULL && strcmp(req, cmd->longCmd) == 0)
        {
            return cmd;
        }
    }

    return NULL;
}

static ResultCode CmdHelp(State* state, StrataCompiler* compiler)
{
    /* Show preamble */
    fprintf(stderr, "stratac - Strata compiler\n"
                    "Usage: stratac [options] <file.strata>\n"
                    "Emits a relocatable object file (.o) by default.\n"
                    "Options:\n");

    const int tmpBufferSize = 512;
    char tmpCmdBuffer[tmpBufferSize];

    const int numCommands = sizeof(commands) / sizeof(commands[0]);
    for (int i = 0; i < numCommands; i++)
    {
        const CLICommand* cmd = &commands[i];

        /* Only one command format is available (only short or only long) */
        if (cmd->shortCmd == NULL || cmd->longCmd == NULL)
        {
            const char* cmdText = (cmd->shortCmd) ? cmd->shortCmd : cmd->longCmd;

            if (cmd->follower != NULL)
            {
                snprintf(tmpCmdBuffer, tmpBufferSize, "%s %s", cmdText, cmd->follower);
            }
            else
            {
                snprintf(tmpCmdBuffer, tmpBufferSize, "%s", cmdText);
            }
        }
        /* Both formats are provided */
        else
        {
            if (cmd->follower != NULL)
            {
                snprintf(tmpCmdBuffer, tmpBufferSize, "%s, %s %s", cmd->shortCmd, cmd->longCmd, cmd->follower);
            }
            else
            {
                snprintf(tmpCmdBuffer, tmpBufferSize, "%s, %s", cmd->shortCmd, cmd->longCmd);
            }
        }

        fprintf(stderr, "  %-22s%s\n", tmpCmdBuffer, cmd->description);
    }

    unsigned capabilities = strataCapabilities();

    if (!(capabilities & STRATA_CAP_LLVM_AOT))
    {
        fprintf(stderr, "This build has no LLVM object/assembly backend.\n");
    }
    if (!(capabilities & STRATA_CAP_TCC_JIT))
    {
        fprintf(stderr, "This build has no in-memory TinyCC backend.\n");
    }

    return RCSuccess;
}

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
    const char* outputFileName;
    const char* sourceFileName;

    bool emitAsm;
    bool printAst;
    bool emitC;
    bool emitLlvmIr;
    bool run;
    bool outFileOwned;
    const char* entryName;
    StrataArch outputArch;
    StrataEmitFlags emitFlags;

    char** arguments;
    int* argumentIndex;
    int argumentCount;
} State;

void StateDefault(State* c)
{
    memset(c, 0, sizeof(State));

    c->outputFileName = NULL;
    c->sourceFileName = NULL;

    c->emitAsm = false;
    c->printAst = false;
    c->emitC = false;
    c->emitLlvmIr = false;
    c->run = false;
    c->outFileOwned = false;
    c->entryName = "main";

    c->outputArch = STRATA_ARCH_AUTO;
    c->emitFlags = 0;

    c->arguments = NULL;
    c->argumentIndex = NULL;
    c->argumentCount = 0;
}

static ResultCode CmdVersion(State* state, StrataCompiler* compiler)
{
    unsigned capabilities = strataCapabilities();

    printf("stratac 0.1.0 (C%s%s", capabilities & STRATA_CAP_TCC_JIT ? ", TinyCC JIT" : "",
           capabilities & STRATA_CAP_LLVM_AOT ? ", LLVM " : "");
    if (capabilities & STRATA_CAP_LLVM_AOT)
    {
        printf("%s", strataLLVMVersion());
    }
    printf(")\n");

    return RCSuccess;
}

static ResultCode CmdOutputFileName(State* state, StrataCompiler* compiler)
{
    if ((*state->argumentIndex) + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: -o needs an argument\n");
        return RCArgumentError;
    }

    (*state->argumentIndex)++;
    state->outputFileName = state->arguments[*state->argumentIndex];

    return RCSuccess;
}

static ResultCode CmdEmitAsm(State* state, StrataCompiler* compiler)
{
    return RCSuccess;
}

static ResultCode CmdPrintAst(State* state, StrataCompiler* compiler)
{
    return RCSuccess;
}

static ResultCode CmdEmitC(State* state, StrataCompiler* compiler)
{
    if (state->emitAsm)
    {
        fprintf(stderr, "error: --asm cannot be combined with --emit-c\n");
        return RCArgumentError;
    }

    if (!state->outputFileName)
    {
        state->outputFileName = ReplaceExt(state->sourceFileName, ".c");
        state->outFileOwned = true;
    }

    StrataResult result = strataCompileFile(compiler, state->sourceFileName, STRATA_EMIT_C, state->emitFlags);
    if (result.diagnostics && result.diagnostics[0])
    {
        fprintf(stderr, "%s\n", result.diagnostics);
    }
    if (!result.ok)
    {
        strataResultFree(&result);

        if (state->outFileOwned)
        {
            free((void*)state->outputFileName);
        }

        return RCIOError;
    }

    FILE* output = fopen(state->outputFileName, "wb");

    if (output == NULL)
    {
        fprintf(stderr, "error: cannot open output '%s'\n", state->outputFileName);
        strataResultFree(&result);

        if (state->outFileOwned)
        {
            free((void*)state->outputFileName);
        }

        return RCIOError;
    }

    size_t outputLen = strlen(result.output);
    bool wrote = fwrite(result.output, 1, outputLen, output) == outputLen;

    strataResultFree(&result);

    if (!wrote || fclose(output) != 0)
    {
        fprintf(stderr, "error: failed writing C source '%s'\n", state->outputFileName);

        if (state->outFileOwned)
        {
            free((void*)state->outputFileName);
        }

        return RCIOError;
    }

    fprintf(stderr, "wrote C source: %s\n", state->outputFileName);

    if (state->outFileOwned)
    {
        free((void*)state->outputFileName);
    }

    return RCSuccess;
}

static ResultCode CmdRun(State* state, StrataCompiler* compiler)
{
    if (!(strataCapabilities() & STRATA_CAP_TCC_JIT))
    {
        fprintf(stderr, "error: TinyCC JIT backend not built\n");
        return RCIOError;
    }

    const char* error = NULL;
    StrataJit* jit = strataJitCompileFile(compiler, state->sourceFileName, &error);
    if (!jit)
    {
        fprintf(stderr, "%s\n", error ? error : "JIT compilation failed");
        strataFree((char*)error);
        return RCIOError;
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
        return RCIOError;
    }

    if (!strataJitCanInvokeIntVoid(jit, state->entryName))
    {
        fprintf(stderr, "error: entry '%s' must be a defined int(void) function\n", state->entryName);
        strataJitDestroy(jit);
        return RCIOError;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, state->entryName);
    if (!entry)
    {
        fprintf(stderr, "error: entry '%s' was not found\n", state->entryName);
        strataJitDestroy(jit);
        return RCIOError;
    }

    int exitCode = entry();

    return exitCode;
}

int main(int argc, char** argv)
{
    State state;
    StateDefault(&state);

    state.arguments = argv;
    state.argumentCount = argc;

    StrataCompiler* compiler = strataCompilerCreate();

    for (int i = 1; i < argc; i++)
    {
        state.argumentIndex = &i;

        const char* a = argv[i];

        const CLICommand* foundCommand = FindCommand(a, commands, sizeof(commands) / sizeof(commands[0]));

        if (foundCommand)
        {
            ResultCode result = foundCommand->func(&state, compiler);

            /* Return if there was an error or the command is marked final */
            if (result != RCSuccess || (foundCommand->flags & CF_FINAL))
            {
                strataCompilerDestroy(compiler);
                return result;
            }

            continue;
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

        // if (strcmp(a, "-o") == 0)
        // {
        //     if (i + 1 >= argc)
        //     {
        //         fprintf(stderr, "error: -o needs an argument\n");
        //         return 2;
        //     }

        //     state.outputFileName = argv[++i];
        // }

        else if (strcmp(a, "--arch") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --arch needs an argument (auto, x64, arm64)\n");
                return RCArgumentError;
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
                return RCArgumentError;
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
                return RCArgumentError;
            }
            state.entryName = argv[++i];
        }
        else if (a[0] == '-' && a[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            CmdHelp(&state, compiler);
            return RCArgumentError;
        }
        else
        {
            if (state.sourceFileName)
            {
                fprintf(stderr, "error: multiple input files are not supported\n");
                return RCArgumentError;
            }
            state.sourceFileName = a;
        }
    }

    if (!state.sourceFileName)
    {
        fprintf(stderr, "error: no input file\n");
        CmdHelp(&state, compiler);
        strataCompilerDestroy(compiler);
        return RCArgumentError;
    }

    strataSetArchitecture(compiler, state.outputArch);

    if (!state.run && strcmp(state.entryName, "main") != 0)
    {
        fprintf(stderr, "error: --entry requires --run\n");
        strataCompilerDestroy(compiler);
        return RCArgumentError;
    }

    if (state.run && (state.emitC || state.emitAsm || state.outputFileName))
    {
        fprintf(stderr, "error: --run cannot be combined with --emit-c, --asm, or -o\n");
        strataCompilerDestroy(compiler);
        return RCArgumentError;
    }

    if (state.printAst)
    {
        StrataResult r = strataCompileFile(compiler, state.sourceFileName, STRATA_EMIT_AST, 0);
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
        ResultCode result = CmdRun(&state, compiler);
        strataCompilerDestroy(compiler);
        return result;
    }

    if (state.emitLlvmIr)
    {
    }

    if (state.emitC)
    {
        ResultCode result = CmdEmitC(&state, compiler);
        strataCompilerDestroy(compiler);
        return result;
    }

    if (!state.outputFileName)
    {
        state.outputFileName = ReplaceExt(state.sourceFileName, ".o");
        state.outFileOwned = true;
    }

    const char* err = NULL;
    int ok = strataCompileToObject(compiler, state.sourceFileName, state.outputFileName, 0, &err);

    if (!ok)
    {
        fprintf(stderr, "%s\n", err ? err : "compilation failed");
        strataFree((char*)err);
        strataCompilerDestroy(compiler);
        if (state.outFileOwned)
        {
            free((void*)state.outputFileName);
        }
        return 1;
    }

    fprintf(stderr, "wrote object: %s\n", state.outputFileName);

    if (state.emitAsm)
    {
        char* asmFile = ReplaceExt(state.outputFileName, ".s");
        const char* asmErr = NULL;
        int asmOk = strataCompileToObject(compiler, state.sourceFileName, asmFile, 1, &asmErr);

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
        free((void*)state.outputFileName);
    }

    strataCompilerDestroy(compiler);
    return 0;
}
