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

/* Set host-provided alloc/free used by the JIT runtime
    Note, this is optional: by default, malloc and free will be used.
    Call with null to restore/null malloc. */
void LLVMJitSetAllocFree(LLVMJit* jit, void* allocFn, void* freeFn);

bool LLVMJitLoad(LLVMJit* jit, BuiltModule* bm, char** errorMessage);
bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr);
uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name);

size_t LLVMJitExternCount(const LLVMJit* jit);
const char* LLVMJitExternName(const LLVMJit* jit, size_t index);
