#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "TypeRegistry.h"
#include "Core/Util.h"

enum StrataArch : int;

typedef enum CBackendEmitFlags
{
    CEmitNone = 0,
    CEmitJIT = (1 << 0),
    CEmitEnableSIMD = (1 << 1),
} CBackendEmitFlags;

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

typedef struct CEmitter
{
    const Module* mod;
    DiagnosticEngine* diag;
    Arena* arena;
    TypeRegistry types;
    StrMap symbols;
    Sb out;
    Vec exports;
    Vec externs;
    CBackendEmitFlags emitFlags;
    enum StrataArch arch;
    unsigned indent;
    const SourceManager* sources;
    size_t sourceCount;
    Vec boxVars;               /* in-scope box-local OwnEntry* (current function) */
    bool terminated;           /* the current block ended in a definite return */
    const char* currentReturn; /* current function's return type name */
    unsigned retCounter;
    unsigned boxTmpCounter; /* unique names for inline struct-init field boxing */
} CEmitter;

void BuiltCModuleInit(BuiltCModule* module);
void BuiltCModuleDispose(BuiltCModule* module);

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, CBackendEmitFlags emitFlags, enum StrataArch arch);
BuiltCModule BuildCModuleWithSources(const Module* ast, DiagnosticEngine* diag, Arena* arena,
                                    const SourceManager* sources, size_t sourceCount, CBackendEmitFlags emitFlags, enum StrataArch arch);

void CEmitExpr(CEmitter* emitter, const Node* node);
