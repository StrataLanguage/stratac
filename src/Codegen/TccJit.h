#pragma once

#include "Codegen/CBackend.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct TCCState TCCState;
typedef struct Arena Arena;

typedef struct {
    char* strataName;
    char* cName;
    bool isIntVoid;
} TccJitSymbol;

typedef struct {
    TCCState* state;
    bool relocated;
    Vec exports;
    Vec externs;
    char* diagnostics;
    size_t diagnosticsLen;
    size_t diagnosticsCap;
    Arena* tccArena;
    void* allocFn;   /* host-provided allocator (NULL = malloc) */
    void* freeFn;    /* host-provided deallocator (NULL = free) */
} TccJit;

void TccJitInit(TccJit* jit);
void TccJitDestroy(TccJit* jit);

/* Set host-provided alloc/free used by the JIT runtime (e.g. a game engine's
   custom allocator). Must be called before TccJitLoad. NULL restores malloc. */
void TccJitSetAllocFree(TccJit* jit, void* allocFn, void* freeFn);

bool TccJitLoad(TccJit* jit, const BuiltCModule* module, char** errorMessage);
bool TccJitAddSymbol(TccJit* jit, const char* name, void* address);
void* TccJitGetAddress(TccJit* jit, const char* name);
bool TccJitCanInvokeIntVoid(const TccJit* jit, const char* name);
size_t TccJitExternCount(const TccJit* jit);
const char* TccJitExternName(const TccJit* jit, size_t index);
const char* TccJitDiagnostics(const TccJit* jit);
