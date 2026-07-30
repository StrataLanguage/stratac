// strata.h - public embedding API for the Strata language (C ABI).
//
// Host applications (e.g. a game engine) link against the Strata shared
// library and use these functions to compile Strata source at runtime. The
// interface is pure C so it can be called from C, C++, C#, Rust, or any
// language with a foreign-function interface.
//
// Result strings are owned by the compiler and freed with strataResultFree().
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StrataCompiler StrataCompiler;

typedef enum {
    STRATA_EMIT_LLVM_IR = 0, // textual LLVM IR
    STRATA_EMIT_AST     = 1, // pretty-printed AST
} StrataEmitKind;

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

// Returns the linked LLVM version, or "0.0.0" if built without LLVM.
const char* strataLLVMVersion(void);

#ifdef __cplusplus
}
#endif
