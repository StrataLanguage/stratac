// Source text storage and line and column lookup.

#include "Core/SourceLocation.h"
#include "Core/Util.h"

#include <stdlib.h>
#include <string.h>

void SourceManagerInit(SourceManager* sm)
{
    *sm = (SourceManager){0};
    sm->text = NULL;
    sm->textLen = 0;
    sm->name = "<string>";
    sm->lineStarts = (uint32_t*)malloc(sizeof(uint32_t));
    if (!sm->lineStarts)
    {
        STRATA_OOM();
    }
    sm->lineStarts[0] = 0;
    sm->lineCount = 1;
    sm->lineCap = 1;
}

void SourceManagerFree(SourceManager* sm)
{
    free(sm->lineStarts);
    sm->lineStarts = NULL;
    sm->lineCount = 0;
    sm->lineCap = 0;
}

static void ComputeLineStarts(SourceManager* sm)
{
    sm->lineCount = 0;

    if (!sm->text)
    {
        sm->lineStarts[0] = 0;
        sm->lineCount = 1;
        return;
    }

    if (sm->lineCap == 0)
    {
        sm->lineCap = 64;
        sm->lineStarts = (uint32_t*)malloc(sm->lineCap * sizeof(uint32_t));
        if (!sm->lineStarts)
        {
            STRATA_OOM();
        }
    }

    sm->lineStarts[sm->lineCount++] = 0;

    for (size_t i = 0; i < sm->textLen; ++i)
    {
        char c = sm->text[i];

        if (c == '\n')
        {
            if (sm->lineCount >= sm->lineCap)
            {
                sm->lineCap *= 2;
                uint32_t* grown = (uint32_t*)realloc(sm->lineStarts, sm->lineCap * sizeof(uint32_t));
                if (!grown)
                {
                    STRATA_OOM();
                }
                sm->lineStarts = grown;
            }
            sm->lineStarts[sm->lineCount++] = (uint32_t)(i + 1);
        }
        else if (c == '\r')
        {
            if (i + 1 < sm->textLen && sm->text[i + 1] == '\n')
            {
                continue;
            }

            if (sm->lineCount >= sm->lineCap)
            {
                sm->lineCap *= 2;
                uint32_t* grown = (uint32_t*)realloc(sm->lineStarts, sm->lineCap * sizeof(uint32_t));
                if (!grown)
                {
                    STRATA_OOM();
                }
                sm->lineStarts = grown;
            }
            sm->lineStarts[sm->lineCount++] = (uint32_t)(i + 1);
        }
    }
}

void SourceManagerSetSource(SourceManager* sm, const char* text, size_t len, const char* name)
{
    sm->text = text;
    sm->textLen = len;
    sm->name = name ? name : "<string>";
    ComputeLineStarts(sm);
}

Str SourceManagerSlice(const SourceManager* sm, SourceRange r)
{
    if (r.start >= sm->textLen)
    {
        return STR_EMPTY;
    }

    uint32_t end = SourceRangeEnd(r);
    if (end > sm->textLen)
    {
        end = (uint32_t)sm->textLen;
    }

    return (Str){sm->text + r.start, end - r.start};
}

LineCol SourceManagerLineCol(const SourceManager* sm, uint32_t offset)
{
    size_t idx = UpperBound(sm->lineStarts, sm->lineCount, offset);

    if (idx == 0)
    {
        idx = 1;
    }

    uint32_t lineIdx = (uint32_t)idx - 1;

    return (LineCol){.line = lineIdx + 1, .column = offset - sm->lineStarts[lineIdx] + 1};
}

Str SourceManagerLineText(const SourceManager* sm, uint32_t line)
{
    if (line == 0 || line > sm->lineCount)
    {
        return STR_EMPTY;
    }

    size_t start = sm->lineStarts[line - 1];
    size_t end = (line < sm->lineCount) ? sm->lineStarts[line] : sm->textLen;

    while (end > start && (sm->text[end - 1] == '\n' || sm->text[end - 1] == '\r'))
    {
        --end;
    }

    return (Str){sm->text + start, end - start};
}

uint32_t SourceManagerLineCount(const SourceManager* sm)
{
    return (uint32_t)sm->lineCount;
}
