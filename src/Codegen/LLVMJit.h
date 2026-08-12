#pragma once

#include "LLVMModuleBuilder.h"


typedef struct {
    LLVMExecutionEngineRef m_ee;
    LLVMContextRef m_ctx;
    LLVMModuleRef m_mod;
    Vec m_externs;
} LLVMJit;

void LLVMJitInit(LLVMJit* jit);
void LLVMJitDestroy(LLVMJit* jit);

bool LLVMJitLoad(LLVMJit* jit, BuiltModule* bm, char** errorMessage);
bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr);
uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name);
