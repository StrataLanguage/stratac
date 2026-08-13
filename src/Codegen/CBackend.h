#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "TypeRegistry.h"
#include "Core/Util.h"

#include "strata/strata.h"

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
    int arch;
    unsigned indent;
    const SourceManager* sources;
    size_t sourceCount;
    Vec boxVars;               /* in-scope box-local OwnEntry* (current function) */
    bool terminated;           /* the current block ended in a definite return */
    const char* currentReturn; /* current function's return type name */
    unsigned retCounter;
    unsigned boxTmpCounter; /* unique names for inline struct-init field boxing */
    bool boundsCheck;       /* emit array bounds checks (StrataProfile) */
    bool nullExternCall;    /* panic on calling a null extern slot (StrataProfile) */
} CEmitter;

void BuiltCModuleInit(BuiltCModule* module);
void BuiltCModuleDispose(BuiltCModule* module);

BuiltCModule BuildCModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, CBackendEmitFlags emitFlags, int arch,
                          const StrataProfile* profile);
BuiltCModule BuildCModuleWithSources(const Module* ast, DiagnosticEngine* diag, Arena* arena,
                                    const SourceManager* sources, size_t sourceCount, CBackendEmitFlags emitFlags,
                                    int arch, const StrataProfile* profile);

/* Emits C source for the panic-unwind host-boundary wrappers, plus the
   exports/externs tables (arena-owned; dispose the returned module with
   BuiltCModuleDispose). See the implementation comment in CBackend.c. */
BuiltCModule BuildEntryWrappersC(const Module* ast, DiagnosticEngine* diag, Arena* arena);

void CEmitExpr(CEmitter* emitter, const Node* node);
