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
} StrataEmitKind;

typedef enum StrataEmitFlags
{
    /* Not supported yet: SIMD vector types always lower to native vector
       instructions, so an STRATA_EMIT_LLVM_IR compile with this flag fails
       with an error instead of silently emitting SIMD code. */
    STRATA_EMIT_NO_SIMD = (1U << 0),
} StrataEmitFlags;

typedef enum
{
    STRATA_CAP_LLVM_IR  = 1u << 0,
    STRATA_CAP_LLVM_AOT = 1u << 1,
    STRATA_CAP_LLVM_JIT = 1u << 2,
} StrataCapability;

typedef enum
{
    STRATA_JIT_BACKEND_AUTO = 0,
    STRATA_JIT_BACKEND_LLVM = 1,
} StrataJitBackend;

typedef enum StrataArch
{
    STRATA_ARCH_AUTO,
    STRATA_ARCH_X64,
    STRATA_ARCH_ARM64,
} StrataArch;

typedef struct StrataProfile
{
    unsigned boundsCheck;    /* AOT panics on OOB; JIT calls the panic handler and continues */
    unsigned nullExternCall; /* panic on calling an extern that was never bound */
} StrataProfile;

STRATA_API StrataProfile strataProfileDefault(void);

typedef void (*StrataPanicHandler)(const char* msg);

STRATA_API void strataSetPanicHandler(StrataPanicHandler handler);

typedef struct
{
    const char* text;   /* module source text (non-owning, your responsibility to ensure lifetime remains valid.) */
    size_t      length; /* length of text (in bytes) */
    const char* name;   /* REQUIRED unique module name (copied); same name = same module */
} StrataResolvedModule;

/* Returns 1 if the module was resolved (`out` filled in), 0 if not available.
 * `importerName` is the canonical name of the module performing the import.
 * `importPath` is the path as written in `import X;` (extension not added). */
typedef int (*StrataImportResolverFn)(void* userData,
                                      const char* importerName,
                                      const char* importPath,
                                      StrataResolvedModule* out);

STRATA_API void strataSetImportResolver(StrataCompiler* c, StrataImportResolverFn resolver, void* userData);

/* Adds a directory searched (in the order added) for a disk import that is not found next to the
 * importing file, e.g. a shared library root: `import engine/scene;` from anywhere. Returns 0 on bad
 * input. Ignored when an import resolver is set. */
STRATA_API int strataAddImportDir(StrataCompiler* c, const char* dir);


/* Hidden context: every non-extern function takes a leading `void* ctx`, so
 * `int f(int x)` is `int f(void* ctx, int x)` from C. Each module exports
 * `void* __strata_context_create(void)` and `void __strata_context_destroy(void*)`;
 * a context is one instance of the module's globals (NULL if it has none).
 * Destroy each once, before strataJitDestroy.
 *
 * `strata_*` runtime helpers and all `__strata_*` names are reserved. To link
 * several AOT modules together, give each its own strataSetSymbolPrefix. */

/* Generic externs: `extern<T> R Name(params)` is one host function for every T.
 * T must be a plain-data struct used only as a whole `ref T` / `const ref T`
 * parameter; it's inferred from that argument or named (`HasComponent<Health>(e)`).
 * The host gets each T argument as a pointer plus T's descriptor as a hidden
 * last argument:
 *
 *     extern<T> bool GetComponent(Entity e, ref T value);
 *     bool GetComponent(void* e, void* value, const StrataTypeDesc* type); // host
 */
typedef struct StrataTypeDesc
{
    unsigned long long nameHash;   /* FNV-1a 64 over the name's bytes (no terminator) */
    unsigned long long layoutHash; /* equals StrataTypesStruct::layoutHash for the same layout (strata_types.h) */
    unsigned int size;
    unsigned int alignment;
    /* followed by the NUL-terminated type name: see strataTypeDescName */
} StrataTypeDesc;

static inline const char* strataTypeDescName(const StrataTypeDesc* type)
{
    return (const char*)(type + 1);
}

/* Prefixes every exported symbol (functions, context create/destroy, type
 * metadata); externs are unaffected. `prefix` must be [A-Za-z0-9_] (else returns 0);
 * NULL or "" clears it. strataJitGetFunction still takes unprefixed names. */
STRATA_API int strataSetSymbolPrefix(StrataCompiler* c, const char* prefix);

typedef struct StrataJit StrataJit;

STRATA_API StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                              const char* moduleName, const char** errOut);
STRATA_API StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut);

STRATA_API void* strataJitGetFunction(StrataJit* jit, const char* name);

/* 1 if `name` is a defined function whose user-visible Strata signature is
   int(), ignoring the hidden context parameter: call it as `int (*)(void* ctx)`. */
STRATA_API int strataJitHasIntVoidSignature(StrataJit* jit, const char* name);

STRATA_API int strataJitAddSymbol(StrataJit* jit, const char* name, void* fn);

STRATA_API size_t strataJitGetExternSymbolCount(StrataJit* jit);
STRATA_API const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index);

STRATA_API const char* strataJitDiagnostics(StrataJit* jit);
STRATA_API void strataJitDestroy(StrataJit* jit);

/* The module's type metadata (read it with strata/strata_types.h), or NULL when
 * it declares no `@component` structs. Owned by the JIT; valid until
 * strataJitDestroy. */
STRATA_API const void* strataJitGetTypeMetadata(StrataJit* jit, size_t* outSize);

/* Type metadata without code generation: parses and checks the module, then
 * writes the same bytes the JIT and AOT paths embed. On success returns 1 and
 * sets *outBytes (NULL when the module has no components; free with
 * strataFreeTypeMetadata). On a compile error returns 0 and sets *errOut
 * (free with strataFree). */
STRATA_API int strataCompileTypeMetadata(StrataCompiler* c, const char* path, void** outBytes, size_t* outSize,
                                         const char** errOut);
STRATA_API int strataCompileTypeMetadataString(StrataCompiler* c, const char* source, const char* moduleName,
                                               void** outBytes, size_t* outSize, const char** errOut);
STRATA_API void strataFreeTypeMetadata(void* bytes);

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

/* Target architecture for strataCompileToObject. STRATA_ARCH_AUTO (the
   default) uses the host triple; X64/ARM64 swap the host triple's
   architecture and keep its vendor/OS/environment. The JIT always targets
   the host. */
STRATA_API void strataSetArchitecture(StrataCompiler* c, StrataArch arch);

STRATA_API void strataJitSetAllocFreeFunctions(StrataCompiler* c, void* allocFn, void* freeFn);
STRATA_API void strataJitSetBackend(StrataCompiler* c, StrataJitBackend backend);

/* Configures the runtime checks emitted into JIT-compiled code. Pass
   &strataProfileDefault() (the default) for all checks, or a customized
   profile.
   
   Must be called before strataJitCompileString/strataJitCompileFile. */
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
