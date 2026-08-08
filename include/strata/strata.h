/*
 *  ------------------------------------------------------------------------------------
 *  Public embedding API for the Strata language (C ABI).
 *  ------------------------------------------------------------------------------------
 *         __         __           __          __                __           __
 *        / /\       /\ \         /\ \        / /\              /\ \         / /\
 *       / /  \      \_\ \       /  \ \      / /  \             \_\ \       / /  \
 *      / / /\ \__   /\__ \     / /\ \ \    / / /\ \            /\__ \     / / /\ \
 *     / / /\ \___\ / /_ \ \   / / /\ \_\  / / /\ \ \          / /_ \ \   / / /\ \ \
 *     \ \ \ \/___// / /\ \ \ / / /_/ / / / / /  \ \ \        / / /\ \ \ / / /  \ \ \
 *      \ \ \     / / /  \/_// / /__\/ / / / /___/ /\ \      / / /  \/_// / /___/ /\ \
 *  _    \ \ \   / / /      / / /_____/ / / /_____/ /\ \    / / /      / / /_____/ /\ \
 * /_/\__/ / /  / / /      / / /\ \ \  / /_________/\ \ \  / / /      / /_________/\ \ \
 * \ \/___/ /  /_/ /      / / /  \ \ \/ / /_       __\ \_\/_/ /      / / /_       __\ \_\
 *  \_____\/   \_\/       \/_/    \_\/\_\___\     /____/_/\_\/       \_\___\     /____/_/
 *
 *  ------------------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>

#ifdef _WIN32
#if defined(STRATA_STATIC)
#define STRATA_API
#elif defined(STRATA_EXPORTS)
#define STRATA_API __declspec(dllexport)
#else
#define STRATA_API __declspec(dllimport)
#endif
#else   // !_WIN32
#define STRATA_API
#endif  // _WIN32

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct StrataCompiler StrataCompiler;

typedef enum
{
    STRATA_EMIT_LLVM_IR = 0,
    STRATA_EMIT_AST     = 1,
    STRATA_EMIT_C       = 2,
} StrataEmitKind;

typedef enum StrataEmitFlags : unsigned int
{
    STRATA_EMIT_NO_SIMD = (1U << 0),
} StrataEmitFlags;

typedef enum
{
    STRATA_CAP_C_OUTPUT = 1u << 0,
    STRATA_CAP_TCC_JIT  = 1u << 1,
    STRATA_CAP_LLVM_IR  = 1u << 2,
    STRATA_CAP_LLVM_AOT = 1u << 3,
} StrataCapability;

typedef enum StrataArch : int
{
    STRATA_ARCH_AUTO,
    STRATA_ARCH_X64,
    STRATA_ARCH_ARM64,
} StrataArch;



typedef struct StrataJit StrataJit;

STRATA_API StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                              const char* moduleName, const char** errOut);
STRATA_API StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut);

STRATA_API void* strataJitGetFunction(StrataJit* jit, const char* name);
STRATA_API int strataJitCanInvokeIntVoid(StrataJit* jit, const char* name);

STRATA_API int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn);

STRATA_API size_t strataJitGetExternSymbolCount(StrataJit* jit);
STRATA_API const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index);

STRATA_API const char* strataJitDiagnostics(StrataJit* jit);
STRATA_API void strataJitDestroy(StrataJit* jit);

typedef struct
{
    int ok;
    const char* output;
    const char* diagnostics;
    unsigned error_count;
    unsigned warning_count;
} StrataResult;

STRATA_API StrataCompiler* strataCompilerCreate(void);
STRATA_API void strataCompilerDestroy(StrataCompiler* c);

STRATA_API void strataSetArchitecture(StrataCompiler* c, StrataArch arch);

STRATA_API StrataResult strataCompileString(StrataCompiler* c, const char* source,
                                            const char* moduleName, StrataEmitKind emit, StrataEmitFlags emitFlags);
STRATA_API StrataResult strataCompileFile(StrataCompiler* c, const char* path,
                                          StrataEmitKind emit, StrataEmitFlags emitFlags);

STRATA_API int strataCompileToObject(StrataCompiler* c, const char* inputPath,
                                     const char* outputPath, int assembly,
                                     const char** errOut);

STRATA_API void strataResultFree(StrataResult* r);
STRATA_API void strataFree(char* s);
STRATA_API unsigned strataCapabilities(void);
STRATA_API const char* strataLLVMVersion(void);

#ifdef __cplusplus
}
#endif
