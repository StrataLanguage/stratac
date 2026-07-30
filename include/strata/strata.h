// strata.h - public embedding API for the Strata language (C ABI).
//
// Host applications link against the Strata shared
// library and use these functions to compile Strata source at runtime. The
// interface is pure C so it can be called from C, C++, C#, Rust, or any
// language with a foreign-function interface.
//
// Result strings are owned by the compiler and freed with strataResultFree().
#pragma once

#include <stddef.h>

// clang-format off

#ifdef __cplusplus
extern "C"
{
#endif
    
typedef struct StrataCompiler StrataCompiler;

typedef enum {
    STRATA_EMIT_LLVM_IR = 0, // textual LLVM IR
    STRATA_EMIT_AST     = 1, // pretty-printed AST
} StrataEmitKind;

// ---------------------------------------------------------------------------
// JIT execution
//
// Compile Strata source to native code in-process and obtain raw function
// pointers the host can call. This is how a game engine runs scripts: compile
// at load time, resolve entry points, call them every frame. No external
// toolchain or process spawn is involved. (Requires LLVM linkage.)
typedef struct StrataJit StrataJit;

StrataJit* strataJitCompileString(StrataCompiler* c, const char* source,
                                  const char* moduleName, const char** errOut);
StrataJit* strataJitCompileFile(StrataCompiler* c, const char* path,
                                const char** errOut);

// Resolves `name` to a native function pointer, or NULL. Cast to the matching C
// signature, e.g. ((int(*)(int,int))strataJitGetFunction(jit, "add"))(2, 3).
void* strataJitGetFunction(StrataJit* jit, const char* name);

// --- Host bindings (engine runtime) ---------------------------------------
// A script declares host-provided functions with `extern`, e.g.
//   extern int engine_get_hp(int entity);
// The host must bind each such name to a native function pointer after
// strataJitCompile* and before strataJitGetFunction triggers compilation.
// Returns 1 on success, 0 if the name is not declared in the module.
int    strataJitAddSymbol(StrataJit* jit, const char* name, void* fn);

// Enumerates the `extern` names the script declared, so the host can verify it
// can satisfy them. Names are valid until strataJitDestroy.
size_t      strataJitGetExternSymbolCount(StrataJit* jit);
const char* strataJitGetExternSymbolName(StrataJit* jit, size_t index);

const char* strataJitDiagnostics(StrataJit* jit);
void strataJitDestroy(StrataJit* jit);

typedef struct StrataResult {
    int ok;                 // 1 on success, 0 if errors occurred
    const char* output;     // generated text (IR/AST), or "" ; NUL-terminated
    const char* diagnostics;// human-readable diagnostics, or ""; NUL-terminated
    unsigned error_count;
    unsigned warning_count;
} StrataResult;

// Lifecycle ------------------------------------------------------------------

StrataCompiler* strataCompilerCreate(void);
void strataCompilerDestroy(StrataCompiler* c);

// Compilation ---------------------------------------------------------------
// moduleName may be NULL; defaults to "strata_module".

StrataResult strataCompileString(StrataCompiler* c, const char* source,
                                 const char* moduleName, StrataEmitKind emit);

StrataResult strataCompileFile(StrataCompiler* c, const char* path,
                               StrataEmitKind emit);

// Selects which back-end produces IR. Pass 1 to use the in-process LLVM
// back-end (if the library was built with LLVM linkage), 0 to emit text IR.
// Defaults to LLVM when available.
void strataCompilerUseLLVM(StrataCompiler* c, int enabled);

void strataResultFree(StrataResult* r);

// Frees a string returned by the API (e.g. an errOut from strataJitCompile*).
void strataFree(char* s);

// Returns the linked LLVM version, or "0.0.0" if built without LLVM.
const char* strataLLVMVersion(void);

#ifdef __cplusplus
}

#endif

// clang-format on