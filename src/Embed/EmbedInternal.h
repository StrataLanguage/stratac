// Private declarations shared by the src/Embed/ files, which implement the public API in strata.h.
#pragma once

#include "strata/strata.h"

#include "Codegen/CodegenBackend.h"
#include "Codegen/TypeMetadata.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Sema/ResolveOverloads.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Import/ModuleLoader.h"

#if STRATA_HAS_LLVM
#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMCApi.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    struct StrataCompiler
    {
        StrataArch arch;
        void* allocFn; // optional host allocator for JIT mode
        void* freeFn;  // optional host deallocator for JIT mode
        StrataJitBackend jitBackend;
        StrataProfile profile; // JIT runtime checks (default: all on)

        StrataImportResolverFn importResolver; // optional import resolver
        void* importResolverUserData;

        char* symbolPrefix; // prepended to exported symbols (strataSetSymbolPrefix), NULL = none

        char** importDirs; // strataAddImportDir, searched in order
        size_t importDirCount;
    };

    // Defined in Pipeline.c; shared by the other src/Embed/ files.
    char* ConcatOwned(const char* a, const char* b);
    void SetErrOut(const char** errOut, const char* message, const char* fallback);
    void InitModuleLoader(Arena* arena, DiagnosticEngine* diag, ModuleLoader* loader, StrataCompiler* c);
    void TeardownCompile(Module* mod, ModuleLoader* loader, DiagnosticEngine* diag, Arena* arena);
    void AnalyzeModule(Module* mod, DiagnosticEngine* diag, Arena* arena);

    typedef enum
    {
        STRATA_JIT_KIND_NONE = 0,
        STRATA_JIT_KIND_LLVM,
    } StrataJitKind;

    struct StrataJit
    {
        StrataJitKind kind;
        void* backend; // LLVMJit*, per kind; NULL if kind == NONE
        char* diagnostics;
        char* symbolPrefix;    // exported symbols carry this prefix; lookups add it
        uint8_t* typeMetadata; // strata_types.h blob, NULL without components
        size_t typeMetadataSize;
#if STRATA_HAS_LLVM
        Vec llvmExports; // LlvmJitExport*, only populated when kind == STRATA_JIT_KIND_LLVM
#endif
    };

#if STRATA_HAS_LLVM
    typedef struct
    {
        char* name;
        bool isIntVoid; // user-visible signature is int(void), ignoring any hidden context pointer
    } LlvmJitExport;
#endif

#ifdef __cplusplus
}
#endif
