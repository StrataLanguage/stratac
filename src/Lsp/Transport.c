#include "Lsp/Transport.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// Reads one header line, without its line ending. Returns false at end of input.
static bool ReadHeaderLine(FILE* in, char* buf, size_t cap)
{
    size_t n = 0;
    int c;

    while ((c = fgetc(in)) != EOF)
    {
        if (c == '\n')
        {
            break;
        }
        if (n + 1 < cap)
        {
            buf[n++] = (char)c;
        }
    }

    if (c == EOF && n == 0)
    {
        return false;
    }

    if (n > 0 && buf[n - 1] == '\r')
    {
        n--;
    }
    buf[n] = '\0';
    return true;
}

char* LspReadMessage(FILE* in, size_t* outLen)
{
    size_t contentLength = 0;
    bool haveLength = false;
    char line[256];

    // Headers end at the first empty line.
    while (true)
    {
        if (!ReadHeaderLine(in, line, sizeof(line)))
        {
            return NULL;
        }

        if (line[0] == '\0')
        {
            break;
        }

        if (strncmp(line, "Content-Length:", 15) == 0)
        {
            char* stop = NULL;
            unsigned long long v = strtoull(line + 15, &stop, 10);
            if (stop == line + 15 || v > (size_t)-1 / 2)
            {
                return NULL;
            }
            contentLength = (size_t)v;
            haveLength = true;
        }
    }

    if (!haveLength)
    {
        return NULL;
    }

    char* body = (char*)malloc(contentLength + 1);
    if (!body)
    {
        return NULL;
    }

    if (fread(body, 1, contentLength, in) != contentLength)
    {
        free(body);
        return NULL;
    }

    body[contentLength] = '\0';
    *outLen = contentLength;
    return body;
}

void LspWriteMessage(FILE* out, const char* body, size_t len)
{
    fprintf(out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, out);
    fflush(out);
}
