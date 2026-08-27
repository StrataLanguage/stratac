#pragma once

#include "LLVMModuleBuilder.h"


typedef struct {
    LLVMOrcLLJITRef m_jit;
    Vec m_externs;
    void* allocFn;   /* host-provided allocator (NULL = malloc) */
    void* freeFn;    /* host-provided deallocator (NULL = free) */
} LLVMJit;

void LLVMJitInit(LLVMJit* jit);
void LLVMJitDestroy(LLVMJit* jit);

/* Optionally override the JIT runtime allocator (NULL restores malloc/free). */
void LLVMJitSetAllocFree(LLVMJit* jit, void* allocFn, void* freeFn);

bool LLVMJitLoad(LLVMJit* jit, BuiltModule* bm, char** errorMessage);
bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr);
uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name);

size_t LLVMJitExternCount(const LLVMJit* jit);
const char* LLVMJitExternName(const LLVMJit* jit, size_t index);
