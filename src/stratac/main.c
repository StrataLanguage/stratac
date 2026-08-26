#include "strata/strata.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replaces a path's file extension (or appends one if it has none) and
   returns a freshly malloc'd string. Local to the CLI so it only depends on
   the public API. */
static char* ReplaceExt(const char* path, const char* ext)
{
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    const char* lastSep = bslash > slash ? bslash : slash;

    const char* dot = strrchr(path, '.');

    if (dot && (!lastSep || dot > lastSep))
    {
        size_t baseLen = (size_t)(dot - path);
        size_t extLen = strlen(ext);
        char* result = (char*)malloc(baseLen + extLen + 1);
        memcpy(result, path, baseLen);
        memcpy(result + baseLen, ext, extLen + 1);
        return result;
    }

    size_t len = strlen(path);
    size_t extLen = strlen(ext);
    char* result = (char*)malloc(len + extLen + 1);
    memcpy(result, path, len);
    memcpy(result + len, ext, extLen + 1);
    return result;
}

/*
 * Command builder helpers
 */

/* Adds an unlabelled separator between options */
#define COMMAND_SEPARATOR {NULL, NULL, NULL, NULL, CF_SEPARATOR, MF_NONE, NULL}
/* Adds a labelled separator between options */
#define COMMAND_SEPARATOR_LABEL(label_) {NULL, NULL, NULL, NULL, CF_SEPARATOR, MF_NONE, label_}

/* Defines a mode that takes no arguments. (e.g. --run) */
#define COMMAND_MODE(long_cmd_, toggle_flag_, desc_) {NULL, long_cmd_, NULL, &Cmd_SetMode, CF_NONE, toggle_flag_, desc_}

/* Same as `COMMAND_MODE`, used for readability */
#define COMMAND_TOGGLE(long_cmd_, toggle_flag_, desc_)                                                                 \
    {NULL, long_cmd_, NULL, &Cmd_SetMode, CF_NONE, toggle_flag_, desc_}

/* Defines a command that writes out information before terminating. (e.g. --version or --help) */
#define COMMAND_INFO(short_cmd_, long_cmd_, func_, desc_) {short_cmd_, long_cmd_, NULL, func_, CF_FINAL, MF_NONE, desc_}

/* Defines a general command used to modify internal state or specify values. (e.g. --arch, -o) */
#define COMMAND_GENERAL(short_cmd_, long_cmd_, follower_, func_, desc_)                                                \
    {short_cmd_, long_cmd_, follower_, func_, CF_NONE, MF_NONE, desc_}

/*
 *
 */

typedef enum ResultCode
{
    RCSuccess = 0,
    RCIOError = 1,
    RCArgumentError = 2,
} ResultCode;

typedef uint64_t ModeFlag;

/* Modes and toggles to be executed after all arguments are parsed. */
typedef enum MOde : uint64_t
{
    MF_NONE,
    MF_PRINT_AST,
    MF_EMIT_ASM,
    MF_EMIT_IR,
    MF_RUN,
} Mode;

#define MF_BIT(cmd_) (1 << (cmd_))
#define HAS_TOGGLE(modes_, cmd_) (((modes_) & MF_BIT(cmd_)) != 0)

typedef struct State
{
    const char* outputFileName;
    const char* sourceFileName;

    bool outFileOwned;
    const char* entryName;
    StrataArch outputArch;
    StrataEmitFlags emitFlags;
    ModeFlag toggleCommands;

    char** arguments;
    int argumentIndex;
    int argumentCount;
} State;

void StateDefault(State* c)
{
    memset(c, 0, sizeof(State));

    c->outputFileName = NULL;
    c->sourceFileName = NULL;

    c->outFileOwned = false;
    c->entryName = "main";

    c->outputArch = STRATA_ARCH_AUTO;
    c->emitFlags = 0;
    c->toggleCommands = MF_NONE;

    c->arguments = NULL;
    c->argumentIndex = 0;
    c->argumentCount = 0;
}

typedef enum CommandFlags
{
    CF_NONE = 0,
    /* The program should exit after this command */
    CF_FINAL = (1 << 0),

    CF_SEPARATOR = (1 << 1),
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

    Mode modeValue;

    const char* description;
} CLICommand;

/*
 * Command Definitions
 */

static ResultCode Cmd_Version(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_Help(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_SetOutputFilename(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_PrintAst(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_SetMode(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_SetArch(State* state, StrataCompiler* compiler, const CLICommand* cmd);
static ResultCode Cmd_DisableSimd(State* state, StrataCompiler* compiler, const CLICommand* cmd);

static ResultCode Cmd_RunSetEntry(State* state, StrataCompiler* compiler, const CLICommand* cmd);

// clang-format off
static const CLICommand commands[] = {
    COMMAND_INFO("-h", "--help",    &Cmd_Help,    "show available commands and usage"),
    COMMAND_INFO("-v", "--version", &Cmd_Version, "print version and exit"),

    COMMAND_GENERAL("-o", NULL,    "<file>", &Cmd_SetOutputFilename, "output object file (default: <input>.o)"),

    COMMAND_MODE("--ast",     MF_PRINT_AST, "print ast tree"),
    COMMAND_MODE("--asm",     MF_EMIT_ASM,  "output asm representation"),
        COMMAND_MODE("--emit-ir", MF_EMIT_IR,   "emit LLVM IR"),
    COMMAND_MODE("--run",     MF_RUN,       "JIT and run an int(void) entry in memory"),

    COMMAND_GENERAL(NULL, "--no-simd", NULL,      &Cmd_DisableSimd, "disable SIMD intrinsics"),
    COMMAND_GENERAL(NULL, "--entry",   "<name>",  &Cmd_RunSetEntry, "entry for --run (default: main)"),
    COMMAND_GENERAL(NULL, "--arch",    "<value>", &Cmd_SetArch,     "set output architecture (default: auto, x64, arm64)"),
};
// clang-format on

/*
 * Command implementations. This run after all of the commands and values are processed, and is mapped directly to
 * `ModeFlag`.
 */
typedef struct ModeImpl
{
    ModeFlag toggle;
    ResultCode (*func)(State* state, StrataCompiler* compiler);
} ModeImpl;

static ResultCode Impl_EmitAsm(State* state, StrataCompiler* compiler);
static ResultCode Impl_EmitIr(State* state, StrataCompiler* compiler);
static ResultCode Impl_JitAndRun(State* state, StrataCompiler* compiler);
static ResultCode Impl_CompileToObject(State* state, StrataCompiler* compiler);
static ResultCode Impl_PrintAst(State* state, StrataCompiler* compiler);

/* The implementations for each mode. Note that the higher the command in this list, the higher the precedence (and
 * therefore will be executed earlier.) */
static const ModeImpl modeImpls[] = {
    {MF_PRINT_AST, &Impl_PrintAst },
    {MF_RUN,       &Impl_JitAndRun},
    {MF_EMIT_ASM,  &Impl_EmitAsm  },
    {MF_EMIT_IR,   &Impl_EmitIr   },
};

const CLICommand* FindCommand(const char* req, const CLICommand* cmds, int count)
{
    for (int i = 0; i < count; i++)
    {
        const CLICommand* cmd = &cmds[i];
        if ((cmd->flags & CF_SEPARATOR) != 0)
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

/**
 * @brief Executes all mode commands that were found during argument parsing
 */
void ExecuteCommands(State* state, StrataCompiler* compiler)
{
    for (int toggleIndex = 0; toggleIndex < sizeof(modeImpls) / sizeof(modeImpls[0]); toggleIndex++)
    {
        const ModeImpl* impl = &modeImpls[toggleIndex];
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
    if ((state->toggleCommands & MF_BIT(MF_EMIT_ASM)) == 0)
    {
        ResultCode result = Impl_CompileToObject(state, compiler);

        if (result != RCSuccess)
        {
            strataCompilerDestroy(compiler);
            exit(result);
        }
    }
}

static ResultCode Cmd_Help(State* state, StrataCompiler* compiler, const CLICommand* cmd)
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

        if ((cmd->flags & CF_SEPARATOR) != 0 && cmd->shortCmd == NULL && cmd->longCmd == NULL)
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
    return RCSuccess;
}

static ResultCode Cmd_Version(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    unsigned capabilities = strataCapabilities();

    printf("stratac 0.1.0 (%s", capabilities & STRATA_CAP_LLVM_AOT ? "LLVM " : "");
    if (capabilities & STRATA_CAP_LLVM_AOT)
    {
        printf("%s", strataLLVMVersion());
    }
    printf(")\n");

    return RCSuccess;
}

static ResultCode Cmd_SetOutputFilename(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    if (state->argumentIndex + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: -o needs an argument\n");
        return RCArgumentError;
    }

    state->outputFileName = state->arguments[++state->argumentIndex];

    return RCSuccess;
}

static ResultCode Cmd_SetMode(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    state->toggleCommands |= MF_BIT(cmd->modeValue);
    return RCSuccess;
}

static ResultCode Cmd_SetArch(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    if (state->argumentIndex + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: --arch needs an argument (x64, arm64, auto)\n");
        return RCArgumentError;
    }

    const char* value = state->arguments[++state->argumentIndex];

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

static ResultCode Cmd_DisableSimd(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    state->emitFlags |= STRATA_EMIT_NO_SIMD;
    return RCSuccess;
}

static ResultCode Cmd_RunSetEntry(State* state, StrataCompiler* compiler, const CLICommand* cmd)
{
    if (state->argumentIndex + 1 >= state->argumentCount)
    {
        fprintf(stderr, "error: --entry needs an argument\n");
        return RCArgumentError;
    }

    state->entryName = state->arguments[++state->argumentIndex];

    return RCSuccess;
}

static ResultCode Impl_PrintAst(State* state, StrataCompiler* compiler)
{
    StrataResult r = strataCompileFile(compiler, state->sourceFileName, STRATA_EMIT_AST, state->emitFlags);
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

static ResultCode Impl_EmitAsm(State* state, StrataCompiler* compiler)
{
    const char* asmFilename = state->outputFileName;

    if (asmFilename == NULL)
    {
        asmFilename = ReplaceExt(state->sourceFileName, ".s");
    }
    const char* asmErr = NULL;
    int asmOk = strataCompileToObject(compiler, state->sourceFileName, asmFilename, true, &asmErr);

    if (!asmOk)
    {
        fprintf(stderr, "error writing assembly: %s\n", asmErr ? asmErr : "(no message)");
        strataFree((char*)asmErr);
    }
    else
    {
        fprintf(stderr, "wrote assembly: %s\n", asmFilename);
    }

    free((void*)asmFilename);

    return RCSuccess;
}

static ResultCode Impl_EmitIr(State* state, StrataCompiler* compiler)
{
    StrataResult r = strataCompileFile(compiler, state->sourceFileName, STRATA_EMIT_LLVM_IR, state->emitFlags);
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

static ResultCode Impl_JitAndRun(State* state, StrataCompiler* compiler)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        fprintf(stderr, "error: LLVM JIT backend not built\n");
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

        if (state->outFileOwned)
        {
            free((void*)state->outputFileName);
        }

        /* Ownership of `compiler` stays with main()/ExecuteCommands, which
           destroy it on the non-success path. Do NOT destroy it here or the
           caller frees it a second time (double free). */
        return RCArgumentError;
    }

    return RCSuccess;
}

int main(int argc, char** argv)
{
    State state;
    StateDefault(&state);

    state.arguments = argv;
    state.argumentCount = argc;

    StrataCompiler* compiler = strataCompilerCreate();

    for (state.argumentIndex = 1; state.argumentIndex < argc; state.argumentIndex++)
    {
        const char* argument = argv[state.argumentIndex];

        const CLICommand* foundCommand = FindCommand(argument, commands, sizeof(commands) / sizeof(commands[0]));

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
        else if (argument[0] == '-' && argument[1] != '\0')
        {
            fprintf(stderr, "error: unknown option '%s'\n", argument);
            Cmd_Help(&state, compiler, NULL);
            return RCArgumentError;
        }
        else
        {
            if (state.sourceFileName)
            {
                fprintf(stderr, "error: multiple input files are not supported\n");
                return RCArgumentError;
            }

            state.sourceFileName = argument;
        }
    }

    if (!state.sourceFileName)
    {
        fprintf(stderr, "error: no input file\n");
        Cmd_Help(&state, compiler, NULL);
        strataCompilerDestroy(compiler);
        return RCArgumentError;
    }

    ExecuteCommands(&state, compiler);
    strataCompilerDestroy(compiler);

    return RCSuccess;
}
