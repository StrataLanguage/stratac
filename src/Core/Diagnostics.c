// Diagnostic collection and reporting.

#include "Core/Diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const char* SeverityName(DiagSeverity s)
{
    switch (s)
    {
    case SevError:
        return "error";
    case SevWarning:
        return "warning";
    case SevNote:
        return "note";
    }
    return "error";
}

void DiagnosticEngineInit(DiagnosticEngine* diag)
{
    ArenaInit(&diag->arena, 0);
    diag->diagnostics = NULL;
    diag->count = 0;
    diag->cap = 0;
    diag->errorCount = 0;
}

void DiagnosticEngineFree(DiagnosticEngine* diag)
{
    ArenaFree(&diag->arena);
    free(diag->diagnostics);
    diag->diagnostics = NULL;
    diag->count = 0;
    diag->cap = 0;
    diag->errorCount = 0;
}

static void DiagPushBack(DiagnosticEngine* diag, Diagnostic d)
{
    if (diag->count >= diag->cap)
    {
        size_t newcap = diag->cap ? diag->cap * 2 : 16;

        diag->diagnostics = (Diagnostic*)realloc(diag->diagnostics, newcap * sizeof(Diagnostic));
        if (!diag->diagnostics)
        {
            STRATA_OOM();
        }

        diag->cap = newcap;
    }

    diag->diagnostics[diag->count++] = d;
}

void DiagReport(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* message)
{
    if (severity == SevError)
    {
        diag->errorCount++;
    }

    DiagPushBack(diag, (Diagnostic){
                           .severity = severity,
                           .range = range,
                           .message = ArenaStrdup(&diag->arena, message),
                       });
}

void DiagReportFmt(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char* msg = ArenaVFormat(&diag->arena, fmt, args);
    va_end(args);

    if (severity == SevError)
    {
        diag->errorCount++;
    }

    DiagPushBack(diag, (Diagnostic){
                           .severity = severity,
                           .range = range,
                           .message = msg ? msg : "",
                       });
}

void DiagError(DiagnosticEngine* diag, SourceRange range, const char* message)
{
    DiagReport(diag, SevError, range, message);
}

void DiagWarning(DiagnosticEngine* diag, SourceRange range, const char* message)
{
    DiagReport(diag, SevWarning, range, message);
}

void DiagNote(DiagnosticEngine* diag, SourceRange range, const char* message)
{
    DiagReport(diag, SevNote, range, message);
}

void DiagErrorFmt(DiagnosticEngine* diag, SourceRange range, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char* msg = ArenaVFormat(&diag->arena, fmt, args);

    va_end(args);

    DiagReport(diag, SevError, range, msg ? msg : "");
}

void DiagNoteFmt(DiagnosticEngine* diag, SourceRange range, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char* msg = ArenaVFormat(&diag->arena, fmt, args);

    va_end(args);

    DiagReport(diag, SevNote, range, msg ? msg : "");
}

uint32_t DiagErrorCount(const DiagnosticEngine* diag)
{
    return diag->errorCount;
}

bool DiagHasErrors(const DiagnosticEngine* diag)
{
    return diag->errorCount > 0;
}

size_t DiagCount(const DiagnosticEngine* diag)
{
    return diag->count;
}

void DiagClear(DiagnosticEngine* diag)
{
    diag->count = 0;
    diag->errorCount = 0;
}

void DiagTruncate(DiagnosticEngine* diag, size_t count)
{
    while (diag->count > count)
    {
        diag->count--;

        if (diag->diagnostics[diag->count].severity == SevError && diag->errorCount > 0)
        {
            diag->errorCount--;
        }
    }
}

char* DiagFormat(const DiagnosticEngine* diag, const SourceManager* sources, size_t sourceCount, Arena* arena)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < diag->count; i++)
    {
        const Diagnostic* d = &diag->diagnostics[i];

        const SourceManager* src = (sourceCount > 0) ? &sources[0] : NULL;
        if (d->range.fileId < sourceCount)
        {
            src = &sources[d->range.fileId];
        }

        LineCol lc = src ? SourceManagerLineCol(src, d->range.start) : (LineCol){0, 0};
        const char* fileName = src ? src->name : "<unknown>";

        SbPrintf(&sb, "%s(%u,%u): %s: ", fileName, lc.line, lc.column, SeverityName(d->severity));
        SbPuts(&sb, d->message);
        SbPutc(&sb, '\n');

        if (d->range.length > 0 && src)
        {
            LineCol endLc = SourceManagerLineCol(src, SourceRangeEnd(d->range) - 1);

            if (endLc.line == lc.line)
            {
                Str lt = SourceManagerLineText(src, lc.line);
                SbPutn(&sb, lt.data, lt.len);
                SbPutc(&sb, '\n');
                SbPutr(&sb, ' ', lc.column - 1);
                SbPutc(&sb, '^');

                size_t carets = d->range.length - 1;

                if (carets > 60)
                {
                    carets = 60;
                }

                SbPutr(&sb, '~', carets);
                SbPutc(&sb, '\n');
            }
        }
    }

    return SbFinish(&sb, arena);
}
