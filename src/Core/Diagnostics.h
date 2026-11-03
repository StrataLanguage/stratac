#pragma once

#include "Core/SourceLocation.h"
#include "Core/Util.h"

typedef enum
{
    SevError,
    SevWarning,
    SevNote,
} DiagSeverity;

typedef struct
{
    DiagSeverity severity;
    SourceRange range;
    const char* message;
} Diagnostic;

typedef struct
{
    Arena arena;
    Diagnostic* diagnostics;
    size_t count;
    size_t cap;
    uint32_t errorCount;
} DiagnosticEngine;

void DiagnosticEngineInit(DiagnosticEngine* diag);
void DiagnosticEngineFree(DiagnosticEngine* diag);

void DiagReport(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* message);
void DiagReportFmt(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* fmt, ...);

void DiagError(DiagnosticEngine* diag, SourceRange range, const char* message);
void DiagWarning(DiagnosticEngine* diag, SourceRange range, const char* message);
void DiagNote(DiagnosticEngine* diag, SourceRange range, const char* message);

void DiagErrorFmt(DiagnosticEngine* diag, SourceRange range, const char* fmt, ...);
void DiagNoteFmt(DiagnosticEngine* diag, SourceRange range, const char* fmt, ...);

uint32_t DiagErrorCount(const DiagnosticEngine* diag);
bool DiagHasErrors(const DiagnosticEngine* diag);
size_t DiagCount(const DiagnosticEngine* diag);

void DiagClear(DiagnosticEngine* diag);
// Drops every diagnostic reported after the first `count` (rolls back a speculative parse).
void DiagTruncate(DiagnosticEngine* diag, size_t count);
char* DiagFormat(const DiagnosticEngine* diag, const SourceManager* sources, size_t sourceCount, Arena* arena);
