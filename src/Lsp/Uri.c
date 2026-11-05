#include "Lsp/Uri.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char kFilePrefix[] = "file://";

static int HexValue(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

char* UriToPath(Arena* arena, const char* uri)
{
    if (!uri || strncmp(uri, kFilePrefix, sizeof(kFilePrefix) - 1) != 0)
    {
        return NULL;
    }

    const char* p = uri + sizeof(kFilePrefix) - 1;

    // Skip an authority ("file://host/path"); editors send an empty one.
    if (*p != '/')
    {
        const char* slash = strchr(p, '/');
        if (!slash)
        {
            return NULL;
        }
        p = slash;
    }

    size_t len = strlen(p);
    char* out = (char*)ArenaAlloc(arena, len + 1);
    size_t n = 0;

    for (size_t i = 0; i < len; i++)
    {
        if (p[i] == '%' && i + 2 < len && HexValue(p[i + 1]) >= 0 && HexValue(p[i + 2]) >= 0)
        {
            out[n++] = (char)(HexValue(p[i + 1]) * 16 + HexValue(p[i + 2]));
            i += 2;
        }
        else
        {
            out[n++] = p[i];
        }
    }
    out[n] = '\0';

    // Windows drive paths arrive as "/C:/dir/file"; drop the leading slash.
    if (n >= 3 && out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':')
    {
        return out + 1;
    }

    return out;
}

char* PathToUri(Arena* arena, const char* path)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, kFilePrefix);

    // Windows drive paths ("C:\dir") become "/C:/dir" so the URI has an absolute path.
    if (path[0] != '/' && path[0] != '\\')
    {
        SbPutc(&sb, '/');
    }

    for (const char* p = path; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\\')
        {
            SbPutc(&sb, '/');
        }
        else if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':')
        {
            SbPutc(&sb, (char)c);
        }
        else
        {
            SbPrintf(&sb, "%%%02X", (unsigned)c);
        }
    }

    return SbFinish(&sb, arena);
}
