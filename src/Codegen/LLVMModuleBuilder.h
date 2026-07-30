#pragma once

#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/Util.h"

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    Vec externSymbols;
} BuiltModule;

void BuiltModuleInit(BuiltModule* bm);
void BuiltModuleDispose(BuiltModule* bm);

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode);
