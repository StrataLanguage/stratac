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

enum StrataArch : int;

void BuiltCModuleInit(BuiltCModule* module);
void BuiltCModuleDispose(BuiltCModule* module);

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode, enum StrataArch arch);
BuiltCModule BuildCModuleWithSources(const Module* ast, DiagnosticEngine* diag, Arena* arena,
                                    const SourceManager* sources, size_t sourceCount, bool jitMode, enum StrataArch arch);
