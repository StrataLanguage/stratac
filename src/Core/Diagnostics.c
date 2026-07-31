#include "strata/Core/Diagnostics.h"

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
    arena_init(&diag->m_arena, 0);
    diag->m_diagnostics = NULL;
    diag->m_count = 0;
    diag->m_cap = 0;
    diag->m_errorCount = 0;
}

void DiagnosticEngineFree(DiagnosticEngine* diag)
{
    arena_free(&diag->m_arena);
    free(diag->m_diagnostics);
    diag->m_diagnostics = NULL;
    diag->m_count = 0;
    diag->m_cap = 0;
    diag->m_errorCount = 0;
}

static void DiagPushBack(DiagnosticEngine* diag, Diagnostic d)
{
    if (diag->m_count >= diag->m_cap)
    {
        size_t newcap = diag->m_cap ? diag->m_cap * 2 : 16;
        diag->m_diagnostics = (Diagnostic*)realloc(diag->m_diagnostics, newcap * sizeof(Diagnostic));
        if (!diag->m_diagnostics)
        {
            abort();
        }
        diag->m_cap = newcap;
    }

    diag->m_diagnostics[diag->m_count++] = d;
}

void DiagReport(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* message)
{
    if (severity == SevError)
    {
        diag->m_errorCount++;
    }

    DiagPushBack(diag, (Diagnostic){
                           .severity = severity,
                           .range = range,
                           .message = arena_strdup(&diag->m_arena, message),
                       });
}

void DiagReportFmt(DiagnosticEngine* diag, DiagSeverity severity, SourceRange range, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char* msg = arena_vformat(&diag->m_arena, fmt, args);
    va_end(args);

    if (severity == SevError)
    {
        diag->m_errorCount++;
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

    char* msg = arena_vformat(&diag->m_arena, fmt, args);

    va_end(args);

    DiagReport(diag, SevError, range, msg ? msg : "");
}

uint32_t DiagErrorCount(const DiagnosticEngine* diag)
{
    return diag->m_errorCount;
}

bool DiagHasErrors(const DiagnosticEngine* diag)
{
    return diag->m_errorCount > 0;
}

size_t DiagCount(const DiagnosticEngine* diag)
{
    return diag->m_count;
}

void DiagClear(DiagnosticEngine* diag)
{
    diag->m_count = 0;
    diag->m_errorCount = 0;
}

char* DiagFormat(const DiagnosticEngine* diag, const SourceManager* src, Arena* arena)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < diag->m_count; i++)
    {
        const Diagnostic* d = &diag->m_diagnostics[i];
        LineCol lc = SourceManagerLineCol(src, d->range.start);

        SbPrintf(&sb, "%s(%u,%u): %s: ", src->m_name, lc.line, lc.column, SeverityName(d->severity));
        SbPuts(&sb, d->message);
        SbPutc(&sb, '\n');

        if (d->range.length > 0)
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
