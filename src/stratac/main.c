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

typedef enum ToggleCommands : uint64_t
{
    TC_NONE,
    TC_EMIT_ASM,
    TC_PRINT_AST,
    TC_EMIT_C,
    TC_RUN,
    TC_DISABLE_SIMD,
} ToggleCommands;

#define TC_BIT(cmd_) (1 << (cmd_))

#define HAS_TOGGLE(toggles_, cmd_) (((toggles_) & TC_BIT(cmd_)) != 0)

typedef struct State State;

typedef enum CommandFlags
{
    CF_NONE = 0,
    /* The program should exit after this command */
    CF_FINAL = (1 << 0),
    CF_IGNORE = (1 << 1),
} CommandFlags;

/*
 * Command definitions
 */

typedef struct CLICommand
{
    /* The short representation of the command, e.g. `-v` */
    const char* shortCmd;
    /* The long version of the command, e.g. `--version` */
    const char* longCmd;
    /* Info that follows after the command text */
    const char* follower;
    /* The implementation */
    ResultCode (*func)(State*, StrataCompiler*, const struct CLICommand*);
    /* */
    CommandFlags flags;

    ToggleCommands toggleValue;

    const char* description;
} CLICommand;

/*
 * Command Definitions
 */

static ResultCode CmdVersion(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdHelp(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdOutputFileName(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdPrintAst(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdToggleSetting(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdSetArch(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode CmdRunSetEntry(State* state, StrataCompiler* compiler, const CLICommand* cmd);

#define COMMAND_SEPARATOR {NULL, NULL, NULL, NULL, CF_IGNORE, NULL}
#define COMMAND_SEPARATOR_LABEL(label_) {NULL, NULL, NULL, NULL, CF_IGNORE, ST_NONE, label_}

#define COMMAND_TOGGLE(long_cmd_, toggle_flag_, desc_)                                                                 \
    {NULL, long_cmd_, NULL, &CmdToggleSetting, CF_NONE, toggle_flag_, desc_}

#define COMMAND_INFO(short_cmd_, long_cmd_, func_, desc_) {short_cmd_, long_cmd_, NULL, func_, CF_FINAL, TC_NONE, desc_}

#define COMMAND_GENERAL(short_cmd_, long_cmd_, follower_, func_, desc_)                                                \
    {short_cmd_, long_cmd_, follower_, func_, CF_NONE, TC_NONE, desc_}

// clang-format off
static const CLICommand commands[] = {
    /* Info commands */
    COMMAND_INFO("-h", "--help",    &CmdHelp,    "show available commands and usage"),
    COMMAND_INFO("-v", "--version", &CmdVersion, "print version and exit"),
    COMMAND_GENERAL("-o", NULL,    "<file>", &CmdOutputFileName, "output object file (default: <input>.o)"),
    COMMAND_GENERAL(NULL, "--ast", NULL,     &CmdPrintAst,       "print ast tree"),

    COMMAND_TOGGLE("--asm",    TC_EMIT_ASM, "output asm representation"),
    COMMAND_TOGGLE("--emit-c", TC_EMIT_C,   "emit C code instead of an object file"),
    COMMAND_TOGGLE("--no-simd", TC_DISABLE_SIMD, "disable SIMD intrinsics"),
    COMMAND_TOGGLE("--run", TC_RUN, "JIT and run an int(void) entry in memory"),

    COMMAND_GENERAL(NULL, "--entry", "<name>", &CmdRunSetEntry, "entry for --run (default: main)"),
    COMMAND_GENERAL(NULL, "--arch", "<value>", &CmdSetArch, "set output architecture (default: auto, x64, arm64)"),
};
// clang-format on

/*
 * Command implementations. This run after all of the commands and values are processed, and is mapped directly to
 * `ToggleCommands`.
 */
typedef struct CmdImpls
{
    ToggleCommands toggle;
    ResultCode (*func)(State* state, StrataCompiler* compiler);
} CmdImpls;

static ResultCode Impl_DisableSimd(State* state, StrataCompiler* compiler);
static ResultCode Impl_EmitAsm(State* state, StrataCompiler* compiler);
static ResultCode Impl_EmitC(State* state, StrataCompiler* compiler);
static ResultCode Impl_JitAndRun(State* state, StrataCompiler* compiler);
static ResultCode Impl_CompileToObject(State* state, StrataCompiler* compiler);

static const CmdImpls cmdImpls[] = {
    {TC_EMIT_ASM,     &Impl_EmitAsm    },

    {TC_DISABLE_SIMD, &Impl_DisableSimd},
    {TC_EMIT_C,       &Impl_EmitC      },
    {TC_RUN,          &Impl_JitAndRun  },
};

const CLICommand* FindCommand(const char* req, const CLICommand* cmds, int count)
{
    for (int i = 0; i < count; i++)
    {
        const CLICommand* cmd = &cmds[i];
        if ((cmd->flags & CF_IGNORE) != 0)
        {
            continue;
        }

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

static ResultCode CmdHelp(State* state, StrataCompiler* compiler, const CLICommand* cmd)
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

        if ((cmd->flags & CF_IGNORE) != 0 && cmd->shortCmd == NULL && cmd->longCmd == NULL)
        {
            if (cmd->description != NULL)
            {
                fprintf(stderr, "\n  %s", cmd->description);
            }

            fprintf(stderr, "\n");
            continue;
        }

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

        fprintf(stderr, "  %-25s%s\n", tmpCmdBuffer, cmd->description);
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
/*
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
                    "  --arch           set the output architecture (default: auto, x64, "
                    "arm64)\n"
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
} */

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
    /* bool emitAsm;
    bool printAst;
    bool emitC;
    bool emitLlvmIr;
    bool run; */
    bool outFileOwned;
    const char* entryName;
    StrataArch outputArch;
    StrataEmitFlags emitFlags;
    ToggleCommands toggleCommands;

    char** arguments;
    int* argumentIndex;
    int argumentCount;
} State;

void StateDefault(State* c)
{
    memset(c, 0, sizeof(State));

    c->outputFileName = NULL;
    c->sourceFileName = NULL;

    /* c->emitAsm = false;
    c->printAst = false;
    c->emitC = false;
    c->emitLlvmIr = false;
    c->run = false; */
    c->outFileOwned = false;
    c->entryName = "main";

    c->outputArch = STRATA_ARCH_AUTO;
    c->emitFlags = 0;
    c->toggleCommands = TC_NONE;

    c->arguments = NULL;
    c->argumentIndex = NULL;
    c->argumentCount = 0;
}

/**
 * @brief Execute all mode commands that were found during argument parsing
 */
void ExecuteCommands(State* state, StrataCompiler* compiler)
{
    for (int toggleIndex = 0; toggleIndex < sizeof(cmdImpls) / sizeof(cmdImpls[0]); toggleIndex++)
    {
        const CmdImpls* impl = &cmdImpls[toggleIndex];
        if (HAS_TOGGLE(state->toggleCommands, impl->toggle))
        {
            ResultCode result = impl->func(state, compiler);

            if (result != RCSuccess)
            {
                strataCompilerDestroy(compiler);
                exit(result);
            }
        }
    }

    /* If there were no emit modes specified, compile and emit the object file */
    if ((state->toggleCommands & (TC_BIT(TC_EMIT_ASM) | TC_BIT(TC_EMIT_C))) == 0)
    {
        ResultCode result = Impl_CompileToObject(state, compiler);

        if (result != RCSuccess)
        {
            strataCompilerDestroy(compiler);
            exit(result);
        }
    }
}

static ResultCode CmdVersion(State* state, StrataCompiler* compiler, const CLICommand* cmd)
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

static ResultCode CmdOutputFileName(State* state, StrataCompiler* compiler, const CLICommand* cmd)
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

static ResultCode CmdToggleSetting(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    state->toggleCommands |= TC_BIT(cmd->toggleValue);
    return RCSuccess;
}

static ResultCode CmdSetArch(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    if ((*state->argumentIndex) + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: --arch needs an argument (x64, arm64, auto)\n");
        return RCArgumentError;
    }

    (*state->argumentIndex)++;
    const char* value = state->arguments[*state->argumentIndex];

    if (strcmp(value, "x64") == 0)
    {
        strataSetArchitecture(compiler, STRATA_ARCH_X64);
    }
    else if (strcmp(value, "arm64") == 0)
    {
        strataSetArchitecture(compiler, STRATA_ARCH_ARM64);
    }
    else if (strcmp(value, "auto") == 0)
    {
        strataSetArchitecture(compiler, STRATA_ARCH_AUTO);
    }
    else
    {
        fprintf(stderr, "error: unknown architecture\n");
        return RCArgumentError;
    }

    return RCSuccess;
}

static ResultCode CmdRunSetEntry(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    if ((*state->argumentIndex) + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: --entry needs an argument\n");
        return RCArgumentError;
    }

    (*state->argumentIndex)++;
    state->entryName = state->arguments[*state->argumentIndex];

    return RCSuccess;
}

static ResultCode CmdPrintAst(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    StrataResult r = strataCompileFile(compiler, state->sourceFileName, STRATA_EMIT_AST, 0);
    if (r.diagnostics && r.diagnostics[0])
    {
        fprintf(stderr, "%s\n", r.diagnostics);
    }
    if (r.ok && r.output)
    {
        fprintf(stderr, "%s\n", r.output);
    }

    strataResultFree(&r);

    return RCSuccess;
}

static ResultCode Impl_EmitC(State* state, StrataCompiler* compiler)
{
    if (HAS_TOGGLE(state->toggleCommands, TC_EMIT_ASM))
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

static ResultCode Impl_EmitAsm(State* state, StrataCompiler* compiler)
{
    char* asmFile = ReplaceExt(state->outputFileName, ".s");
    const char* asmErr = NULL;
    int asmOk = strataCompileToObject(compiler, state->sourceFileName, asmFile, true, &asmErr);

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

    return RCSuccess;
}
static ResultCode Impl_JitAndRun(State* state, StrataCompiler* compiler)
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

static ResultCode Impl_CompileToObject(State* state, StrataCompiler* compiler)
{
    if (state->outputFileName == NULL)
    {
        state->outputFileName = ReplaceExt(state->sourceFileName, ".o");
        state->outFileOwned = true;
    }

    const char* err = NULL;
    int ok = strataCompileToObject(compiler, state->sourceFileName, state->outputFileName, 0, &err);

    if (!ok)
    {
        fprintf(stderr, "%s\n", err ? err : "compilation failed");

        strataFree((char*)err);
        strataCompilerDestroy(compiler);

        if (state->outFileOwned)
        {
            free((void*)state->outputFileName);
        }

        return RCArgumentError;
    }

    return RCSuccess;
}

static ResultCode Impl_DisableSimd(State* state, StrataCompiler* compiler)
{
    state->emitFlags |= STRATA_EMIT_NO_SIMD;
    return RCSuccess;
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
            ResultCode result = foundCommand->func(&state, compiler, foundCommand);

            /* Return if there was an error or the command is marked final */
            if (result != RCSuccess || (foundCommand->flags & CF_FINAL))
            {
                strataCompilerDestroy(compiler);
                return result;
            }

            continue;
        }
        else if (a[0] == '-' && a[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            CmdHelp(&state, compiler, NULL);
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
        CmdHelp(&state, compiler, NULL);
        strataCompilerDestroy(compiler);
        return RCArgumentError;
    }

    ExecuteCommands(&state, compiler);

    strataCompilerDestroy(compiler);
    return 0;
}
