#pragma once

#include "Codegen/CBackend.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct TCCState TCCState;

typedef struct {
    char* strataName;
    char* cName;
    bool isIntVoid;
} TccJitSymbol;

typedef struct {
    TCCState* state;
    Vec exports;
    Vec externs;
    char* diagnostics;
    size_t diagnosticsLen;
    size_t diagnosticsCap;
} TccJit;

void TccJitInit(TccJit* jit);
void TccJitDestroy(TccJit* jit);

bool TccJitLoad(TccJit* jit, const BuiltCModule* module, char** errorMessage);
bool TccJitAddSymbol(TccJit* jit, const char* name, void* address);
void* TccJitGetAddress(TccJit* jit, const char* name);
bool TccJitCanInvokeIntVoid(const TccJit* jit, const char* name);
size_t TccJitExternCount(const TccJit* jit);
const char* TccJitExternName(const TccJit* jit, size_t index);
const char* TccJitDiagnostics(const TccJit* jit);
