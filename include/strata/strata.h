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

typedef enum StrataEmitFlags
{
    STRATA_EMIT_NO_SIMD = (1U << 0),
} StrataEmitFlags;

typedef enum
{
    STRATA_CAP_C_OUTPUT = 1u << 0,
    STRATA_CAP_TCC_JIT  = 1u << 1,
    STRATA_CAP_LLVM_IR  = 1u << 2,
    STRATA_CAP_LLVM_AOT = 1u << 3,
    STRATA_CAP_LLVM_JIT = 1u << 4,
} StrataCapability;

typedef enum
{
    STRATA_JIT_BACKEND_AUTO = 0,
    STRATA_JIT_BACKEND_TCC  = 1,
    STRATA_JIT_BACKEND_LLVM = 2,
} StrataJitBackend;

typedef enum StrataArch
{
    STRATA_ARCH_AUTO,
    STRATA_ARCH_X64,
    STRATA_ARCH_ARM64,
} StrataArch;

/* A set of JIT-mode runtime checks. Each field is enabled (1) by default;
   set a field to 0 to disable that check. Obtained via strataProfileDefault()
   or customized by the host. */
typedef struct StrataProfile
{
    unsigned boundsCheck;    /* array index bounds check + panic on out-of-bounds */
    unsigned nullExternCall; /* panic on calling an extern that was never bound */

    /* LLVM JIT only: on panic, unwind the JIT'd stack frame by frame, freeing
       every owning value (box<T>/string/T[]) held by each frame, until the
       host boundary (the function pointer returned by strataJitGetFunction)
       is reached. The panic handler then runs once, after the unwind; if it
       returns, the entry function returns a zeroed value to the host instead
       of crashing. With this off (or on the TCC JIT / AOT), strata_panic
       behaves as before: the handler runs at the panic site and the process
       aborts if it returns. */
    unsigned panicUnwind;
} StrataProfile;

STRATA_API StrataProfile strataProfileDefault(void);

typedef void (*StrataPanicHandler)(const char* msg);

STRATA_API void strataSetPanicHandler(StrataPanicHandler handler);

typedef struct
{
    const char* text;   /* module source text (borrowed; not freed by the library) */
    size_t      length; /* length of text (in bytes) */
    const char* name;   /* diagnostic name - copied by the library */
} StrataResolvedModule;

/* Returns 1 if the module was resolved (`out` filled in), 0 if not available.
 * `importerName` is the canonical name of the module performing the import.
 * `importPath` is the path as written in `import X;` (extension not added). */
typedef int (*StrataImportResolverFn)(void* userData,
                                      const char* importerName,
                                      const char* importPath,
                                      StrataResolvedModule* out);

STRATA_API void strataSetImportResolver(StrataCompiler* c, StrataImportResolverFn resolver, void* userData);


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

STRATA_API void strataJitSetAllocFreeFunctions(StrataCompiler* c, void* allocFn, void* freeFn);
STRATA_API void strataJitSetBackend(StrataCompiler* c, StrataJitBackend backend);

/* Configures the runtime checks emitted into JIT-compiled code. Pass
   &strataProfileDefault() (the default) for all checks, or a customized
   profile. Must be called before strataJitCompileString/strataJitCompileFile. */
STRATA_API void strataJitSetProfile(StrataCompiler* c, const StrataProfile* profile);

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
