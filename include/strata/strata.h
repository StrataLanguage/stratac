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
    unsigned boundsCheck;    /* array bounds check: AOT panics on OOB; the LLVM JIT notifies the panic handler
                              * (strataSetPanicHandler) and continues - OOB reads yield a dummy value, OOB
                              * writes are no-ops. A handler that longjmps still halts the run. */
    unsigned nullExternCall; /* panic on calling an extern that was never bound */
} StrataProfile;

STRATA_API StrataProfile strataProfileDefault(void);

typedef void (*StrataPanicHandler)(const char* msg);

STRATA_API void strataSetPanicHandler(StrataPanicHandler handler);

typedef struct
{
    const char* text;   /* module source text (non-owning, your responsibility to ensure lifetime remains valid.) */
    size_t      length; /* length of text (in bytes) */
    const char* name;   /* REQUIRED canonical module name (copied). It is the module's identity: imports that
                         * resolve to the same name load once, so distinct modules need distinct names. It is also
                         * the diagnostic name and the `importerName` for the module's own imports. A resolver
                         * that leaves it NULL or empty fails the import with an error. */
} StrataResolvedModule;

/* Returns 1 if the module was resolved (`out` filled in), 0 if not available.
 * `importerName` is the canonical name of the module performing the import.
 * `importPath` is the path as written in `import X;` (extension not added). */
typedef int (*StrataImportResolverFn)(void* userData,
                                      const char* importerName,
                                      const char* importPath,
                                      StrataResolvedModule* out);

STRATA_API void strataSetImportResolver(StrataCompiler* c, StrataImportResolverFn resolver, void* userData);


/*
 * Module-level globals and the hidden context parameter
 * ------------------------------------------------------
 * A module with at least one storage-backed global (any global other than a
 * `const` scalar that folds to a compile-time constant) keeps all of them in
 * a per-instance context. Every non-extern Strata function in such a module
 * then takes a hidden leading `void* ctx` parameter at the ABI level, so a
 * Strata function declared `int f(int x)` is called from C as
 * `int f(void* ctx, int x)`. The module also exports:
 *
 *     void* __strata_context_create(void);    // allocate + run global initializers
 *     void  __strata_context_destroy(void*);  // drop owned globals + free the context
 *
 * Each context is an independent instance of the module's globals; create as
 * many as needed and destroy each exactly once, before strataJitDestroy for
 * JIT modules. A module without such globals has no context parameter and
 * does not export these functions (strataJitGetFunction returns NULL).
 *
 * The names `strata_alloc`, `strata_free`, `strata_panic`, `strata_oob`,
 * `strata_strdup`, `strata_str_eq`, `strata_cstrlen` and every `__strata_*`
 * name are reserved: defining a Strata function with one of them is an error.
 *
 * AOT linking: by default the context functions and the string helpers
 * (strata_strdup, strata_str_eq, strata_cstrlen) have fixed, unprefixed
 * external names, as do the module's own functions, so two objects produced by
 * strataCompileToObject collide at link time. Give each module its own
 * strataSetSymbolPrefix to link several into one executable or DLL.
 */

/* Generic externs: `extern<T> R Name(params)` is ONE host function serving every
 * type T. T may only be a whole parameter type (`const ref T value` or `ref T value`),
 * never the return type, and each call's T must be a plain-data struct (no
 * owning fields). Calls name T explicitly or let it be inferred from a T
 * argument:
 *
 *     extern<T> bool GetComponent(Entity entity, ref T value);
 *     extern<T> bool HasComponent(Entity entity);
 *
 *     Health health;
 *     if (GetComponent(entity, health)) { ... }      // T inferred as Health
 *     if (HasComponent<Health>(entity)) { ... }      // T named explicitly
 *
 * On the host side every T parameter is a pointer (to the caller's storage, or
 * to a temporary for a `const ref T` argument that isn't a variable), and T's
 * descriptor is appended as a hidden last argument:
 *
 *     bool GetComponent(void* entity, void* value, const StrataTypeDesc* type);
 *     bool HasComponent(void* entity, const StrataTypeDesc* type);
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

/* Prepends `prefix` to every symbol a module exports: its functions,
 * __strata_context_create/__strata_context_destroy and the type metadata
 * symbol (see strata_types.h). The string helpers become module-local. Externs
 * are unaffected. `prefix` may only contain [A-Za-z0-9_]; NULL or "" removes it.
 * Returns 0 (leaving the prefix unchanged) for an invalid prefix.
 *
 * Applies to both AOT objects and the JIT. strataJitGetFunction still takes the
 * unprefixed name; an AOT host looks up `<prefix><name>` itself. */
STRATA_API int strataSetSymbolPrefix(StrataCompiler* c, const char* prefix);

typedef struct StrataJit StrataJit;

STRATA_API StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                              const char* moduleName, const char** errOut);
STRATA_API StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path, const char** errOut);

STRATA_API void* strataJitGetFunction(StrataJit* jit, const char* name);

/* 1 if `name` can be called directly as `int (*)(void)`: a defined function
   whose Strata signature is int() in a module WITHOUT a context parameter.
   Returns 0 for every function of a module with a context (see above). */
STRATA_API int strataJitCanInvokeIntVoid(StrataJit* jit, const char* name);

/* 1 if `name` is a defined function whose user-visible Strata signature is
   int(), ignoring the hidden context parameter. When strataJitHasContext is 1
   it must be called as `int (*)(void* ctx)`, otherwise as `int (*)(void)`. */
STRATA_API int strataJitHasIntVoidSignature(StrataJit* jit, const char* name);

/* 1 if the module's functions take the hidden leading context pointer (and
   export __strata_context_create/__strata_context_destroy), else 0. */
STRATA_API int strataJitHasContext(StrataJit* jit);

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
