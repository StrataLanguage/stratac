#include "strata/Core/SourceLocation.h"

#include <stdlib.h>
#include <string.h>

void SourceManagerInit(SourceManager* sm)
{
    *sm = (SourceManager){0};
    sm->m_text = NULL;
    sm->m_textLen = 0;
    sm->m_name = "<string>";
    sm->m_lineStarts = (uint32_t*)malloc(sizeof(uint32_t));
    sm->m_lineStarts[0] = 0;
    sm->m_lineCount = 1;
    sm->m_lineCap = 1;
}

void SourceManagerFree(SourceManager* sm)
{
    free(sm->m_lineStarts);
    sm->m_lineStarts = NULL;
    sm->m_lineCount = 0;
    sm->m_lineCap = 0;
}

static void ComputeLineStarts(SourceManager* sm)
{
    sm->m_lineCount = 0;

    if (sm->m_lineCap == 0)
    {
        sm->m_lineCap = 64;
        sm->m_lineStarts = (uint32_t*)malloc(sm->m_lineCap * sizeof(uint32_t));
    }

    sm->m_lineStarts[sm->m_lineCount++] = 0;

    for (size_t i = 0; i < sm->m_textLen; ++i)
    {
        char c = sm->m_text[i];

        if (c == '\n')
        {
            if (sm->m_lineCount >= sm->m_lineCap)
            {
                sm->m_lineCap *= 2;
                sm->m_lineStarts = (uint32_t*)realloc(sm->m_lineStarts, sm->m_lineCap * sizeof(uint32_t));
            }
            sm->m_lineStarts[sm->m_lineCount++] = (uint32_t)(i + 1);
        }
        else if (c == '\r')
        {
            if (i + 1 < sm->m_textLen && sm->m_text[i + 1] == '\n')
            {
                continue;
            }

            if (sm->m_lineCount >= sm->m_lineCap)
            {
                sm->m_lineCap *= 2;
                sm->m_lineStarts = (uint32_t*)realloc(sm->m_lineStarts, sm->m_lineCap * sizeof(uint32_t));
            }
            sm->m_lineStarts[sm->m_lineCount++] = (uint32_t)(i + 1);
        }
    }
}

void SourceManagerSetSource(SourceManager* sm, const char* text, size_t len, const char* name)
{
    sm->m_text = text;
    sm->m_textLen = len;
    sm->m_name = name ? name : "<string>";
    ComputeLineStarts(sm);
}

Str SourceManagerSlice(const SourceManager* sm, SourceRange r)
{
    if (r.start >= sm->m_textLen)
    {
        return STR_EMPTY;
    }

    uint32_t end = SourceRangeEnd(r);
    if (end > sm->m_textLen)
    {
        end = (uint32_t)sm->m_textLen;
    }

    return (Str){sm->m_text + r.start, end - r.start};
}

static size_t UpperBound(const uint32_t* arr, size_t count, uint32_t val)
{
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (arr[mid] <= val)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

LineCol SourceManagerLineCol(const SourceManager* sm, uint32_t offset)
{
    size_t idx = UpperBound(sm->m_lineStarts, sm->m_lineCount, offset);

    if (idx == 0)
    {
        idx = 1;
    }

    uint32_t lineIdx = (uint32_t)idx - 1;

    return (LineCol){
        .line = lineIdx + 1,
        .column = offset - sm->m_lineStarts[lineIdx] + 1,
    };
}

Str SourceManagerLineText(const SourceManager* sm, uint32_t line)
{
    if (line == 0 || line > sm->m_lineCount)
    {
        return STR_EMPTY;
    }

    size_t start = sm->m_lineStarts[line - 1];
    size_t end = (line < sm->m_lineCount) ? sm->m_lineStarts[line] : sm->m_textLen;

    while (end > start && (sm->m_text[end - 1] == '\n' || sm->m_text[end - 1] == '\r'))
    {
        --end;
    }

    return (Str){sm->m_text + start, end - start};
}

uint32_t SourceManagerLineCount(const SourceManager* sm)
{
    return (uint32_t)sm->m_lineCount;
}
