#pragma once

#include "AST/AST.h"
#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"
#include "Core/Util.h"

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    Vec externSymbols;
} BuiltModule;

void BuiltModuleInit(BuiltModule* bm);
void BuiltModuleDispose(BuiltModule* bm);

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode);
