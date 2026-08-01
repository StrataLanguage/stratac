#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "Core/Util.h"

typedef struct {
    const char* strataName;
    const char* cName;
    bool isIntVoid;
} CBackendSymbol;

typedef struct {
    const char* source;
    Vec exports;
    Vec externs;
} BuiltCModule;

void BuiltCModuleInit(BuiltCModule* module);
void BuiltCModuleDispose(BuiltCModule* module);

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode);
