#include "Lsp/Json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Nesting limit: deep enough for any editor message, shallow enough that recursion cannot exhaust the stack.
#define JSON_MAX_DEPTH 256

typedef struct
{
    Arena* arena;
    const char* p;
    const char* end;
    int depth;
} JsonReader;

static bool ParseValue(JsonReader* r, JValue** out);

static void SkipSpace(JsonReader* r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
    {
        r->p++;
    }
}

static JValue* NewValue(JsonReader* r, JType type)
{
    JValue* v = (JValue*)ArenaAlloc(r->arena, sizeof(JValue));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static void AppendUtf8(Sb* sb, uint32_t cp)
{
    if (cp < 0x80)
    {
        SbPutc(sb, (char)cp);
    }
    else if (cp < 0x800)
    {
        SbPutc(sb, (char)(0xC0 | (cp >> 6)));
        SbPutc(sb, (char)(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        SbPutc(sb, (char)(0xE0 | (cp >> 12)));
        SbPutc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        SbPutc(sb, (char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        SbPutc(sb, (char)(0xF0 | (cp >> 18)));
        SbPutc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        SbPutc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        SbPutc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

// Reads four hex digits at `p`. Returns false if any of them is not a hex digit.
static bool ReadHex4(const char* p, const char* end, uint32_t* out)
{
    if (end - p < 4)
    {
        return false;
    }

    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
        {
            d = (uint32_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = (uint32_t)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            d = (uint32_t)(c - 'A' + 10);
        }
        else
        {
            return false;
        }
        v = (v << 4) | d;
    }

    *out = v;
    return true;
}

// Parses a string token. `r->p` must point at the opening quote.
static bool ParseString(JsonReader* r, char** outStr, size_t* outLen)
{
    if (r->p >= r->end || *r->p != '"')
    {
        return false;
    }
    r->p++;

    Sb sb;
    SbInit(&sb);

    while (true)
    {
        if (r->p >= r->end)
        {
            SbFree(&sb);
            return false;
        }

        unsigned char c = (unsigned char)*r->p;
        if (c == '"')
        {
            r->p++;
            break;
        }

        if (c < 0x20)
        {
            SbFree(&sb);
            return false;
        }

        if (c != '\\')
        {
            SbPutc(&sb, (char)c);
            r->p++;
            continue;
        }

        // Escape sequence.
        if (r->end - r->p < 2)
        {
            SbFree(&sb);
            return false;
        }

        char e = r->p[1];
        r->p += 2;
        switch (e)
        {
        case '"':
            SbPutc(&sb, '"');
            break;
        case '\\':
            SbPutc(&sb, '\\');
            break;
        case '/':
            SbPutc(&sb, '/');
            break;
        case 'b':
            SbPutc(&sb, '\b');
            break;
        case 'f':
            SbPutc(&sb, '\f');
            break;
        case 'n':
            SbPutc(&sb, '\n');
            break;
        case 'r':
            SbPutc(&sb, '\r');
            break;
        case 't':
            SbPutc(&sb, '\t');
            break;
        case 'u':
        {
            uint32_t cp;
            if (!ReadHex4(r->p, r->end, &cp))
            {
                SbFree(&sb);
                return false;
            }
            r->p += 4;

            // A high surrogate must be followed by a low surrogate; together they encode one code point.
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                uint32_t low;
                if (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u' && ReadHex4(r->p + 2, r->end, &low)
                    && low >= 0xDC00 && low <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    r->p += 6;
                }
                else
                {
                    cp = 0xFFFD;
                }
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
            {
                cp = 0xFFFD;
            }

            AppendUtf8(&sb, cp);
            break;
        }
        default:
            SbFree(&sb);
            return false;
        }
    }

    size_t len = sb.len;
    char* s = (char*)ArenaAlloc(r->arena, len + 1);
    if (len > 0)
    {
        memcpy(s, sb.data, len);
    }
    s[len] = '\0';
    SbFree(&sb);

    *outStr = s;
    *outLen = len;
    return true;
}

static bool ParseNumber(JsonReader* r, double* out)
{
    const char* start = r->p;
    while (r->p < r->end
           && (*r->p == '-' || *r->p == '+' || *r->p == '.' || *r->p == 'e' || *r->p == 'E'
               || (*r->p >= '0' && *r->p <= '9')))
    {
        r->p++;
    }

    size_t len = (size_t)(r->p - start);
    if (len == 0)
    {
        return false;
    }

    char* copy = (char*)ArenaAlloc(r->arena, len + 1);
    memcpy(copy, start, len);
    copy[len] = '\0';

    char* stop = NULL;
    double v = strtod(copy, &stop);
    if (stop != copy + len)
    {
        return false;
    }

    *out = v;
    return true;
}

static bool MatchLiteral(JsonReader* r, const char* word)
{
    size_t n = strlen(word);
    if ((size_t)(r->end - r->p) < n || strncmp(r->p, word, n) != 0)
    {
        return false;
    }
    r->p += n;
    return true;
}

static bool ParseArray(JsonReader* r, JValue** out)
{
    r->p++; // '['
    JValue* v = NewValue(r, JArray);

    Vec items;
    VecInit(&items);

    SkipSpace(r);
    if (r->p < r->end && *r->p == ']')
    {
        r->p++;
        *out = v;
        return true;
    }

    while (true)
    {
        JValue* item = NULL;
        SkipSpace(r);
        if (!ParseValue(r, &item))
        {
            free(items.items);
            return false;
        }
        VecPush(&items, item);

        SkipSpace(r);
        if (r->p >= r->end)
        {
            free(items.items);
            return false;
        }
        if (*r->p == ',')
        {
            r->p++;
            continue;
        }
        if (*r->p == ']')
        {
            r->p++;
            break;
        }
        free(items.items);
        return false;
    }

    v->count = items.count;
    v->items = (JValue**)ArenaAlloc(r->arena, (items.count ? items.count : 1) * sizeof(JValue*));
    for (size_t i = 0; i < items.count; i++)
    {
        v->items[i] = (JValue*)items.items[i];
    }
    free(items.items);

    *out = v;
    return true;
}

static bool ParseObject(JsonReader* r, JValue** out)
{
    r->p++; // '{'
    JValue* v = NewValue(r, JObject);

    Vec members;
    VecInit(&members);

    SkipSpace(r);
    if (r->p < r->end && *r->p == '}')
    {
        r->p++;
        *out = v;
        return true;
    }

    while (true)
    {
        SkipSpace(r);
        char* key = NULL;
        size_t keyLen = 0;
        if (!ParseString(r, &key, &keyLen))
        {
            free(members.items);
            return false;
        }

        SkipSpace(r);
        if (r->p >= r->end || *r->p != ':')
        {
            free(members.items);
            return false;
        }
        r->p++;

        JValue* value = NULL;
        SkipSpace(r);
        if (!ParseValue(r, &value))
        {
            free(members.items);
            return false;
        }

        JMember* m = (JMember*)ArenaAlloc(r->arena, sizeof(JMember));
        m->key = key;
        m->value = value;
        VecPush(&members, m);

        SkipSpace(r);
        if (r->p >= r->end)
        {
            free(members.items);
            return false;
        }
        if (*r->p == ',')
        {
            r->p++;
            continue;
        }
        if (*r->p == '}')
        {
            r->p++;
            break;
        }
        free(members.items);
        return false;
    }

    v->count = members.count;
    v->members = (JMember*)ArenaAlloc(r->arena, (members.count ? members.count : 1) * sizeof(JMember));
    for (size_t i = 0; i < members.count; i++)
    {
        v->members[i] = *(JMember*)members.items[i];
    }
    free(members.items);

    *out = v;
    return true;
}

static bool ParseValue(JsonReader* r, JValue** out)
{
    if (r->depth >= JSON_MAX_DEPTH)
    {
        return false;
    }

    SkipSpace(r);
    if (r->p >= r->end)
    {
        return false;
    }

    r->depth++;
    bool ok = false;

    switch (*r->p)
    {
    case '{':
        ok = ParseObject(r, out);
        break;
    case '[':
        ok = ParseArray(r, out);
        break;
    case '"':
    {
        JValue* v = NewValue(r, JString);
        ok = ParseString(r, &v->str, &v->strLen);
        if (ok)
        {
            *out = v;
        }
        break;
    }
    case 't':
        ok = MatchLiteral(r, "true");
        if (ok)
        {
            JValue* v = NewValue(r, JBool);
            v->boolean = true;
            *out = v;
        }
        break;
    case 'f':
        ok = MatchLiteral(r, "false");
        if (ok)
        {
            *out = NewValue(r, JBool);
        }
        break;
    case 'n':
        ok = MatchLiteral(r, "null");
        if (ok)
        {
            *out = NewValue(r, JNull);
        }
        break;
    default:
    {
        JValue* v = NewValue(r, JNumber);
        ok = ParseNumber(r, &v->number);
        if (ok)
        {
            *out = v;
        }
        break;
    }
    }

    r->depth--;
    return ok;
}

bool JsonParse(Arena* arena, const char* text, size_t len, JValue** out)
{
    JsonReader r = {.arena = arena, .p = text, .end = text + len, .depth = 0};

    JValue* root = NULL;
    if (!ParseValue(&r, &root))
    {
        return false;
    }

    SkipSpace(&r);
    if (r.p != r.end)
    {
        return false;
    }

    *out = root;
    return true;
}

const JValue* JsonGet(const JValue* obj, const char* key)
{
    if (!obj || obj->type != JObject)
    {
        return NULL;
    }

    for (size_t i = 0; i < obj->count; i++)
    {
        if (strcmp(obj->members[i].key, key) == 0)
        {
            return obj->members[i].value;
        }
    }

    return NULL;
}

void JsonPutString(Sb* sb, const char* s, size_t len)
{
    SbPutc(sb, '"');
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            SbPuts(sb, "\\\"");
            break;
        case '\\':
            SbPuts(sb, "\\\\");
            break;
        case '\n':
            SbPuts(sb, "\\n");
            break;
        case '\r':
            SbPuts(sb, "\\r");
            break;
        case '\t':
            SbPuts(sb, "\\t");
            break;
        default:
            if (c < 0x20)
            {
                SbPrintf(sb, "\\u%04x", (unsigned)c);
            }
            else
            {
                SbPutc(sb, (char)c);
            }
            break;
        }
    }
    SbPutc(sb, '"');
}

void JsonPutValue(Sb* sb, const JValue* v)
{
    if (!v)
    {
        SbPuts(sb, "null");
        return;
    }

    switch (v->type)
    {
    case JNull:
        SbPuts(sb, "null");
        break;
    case JBool:
        SbPuts(sb, v->boolean ? "true" : "false");
        break;
    case JNumber:
        SbPrintf(sb, "%.17g", v->number);
        break;
    case JString:
        JsonPutString(sb, v->str, v->strLen);
        break;
    case JArray:
        SbPutc(sb, '[');
        for (size_t i = 0; i < v->count; i++)
        {
            if (i > 0)
            {
                SbPutc(sb, ',');
            }
            JsonPutValue(sb, v->items[i]);
        }
        SbPutc(sb, ']');
        break;
    case JObject:
        SbPutc(sb, '{');
        for (size_t i = 0; i < v->count; i++)
        {
            if (i > 0)
            {
                SbPutc(sb, ',');
            }
            JsonPutString(sb, v->members[i].key, strlen(v->members[i].key));
            SbPutc(sb, ':');
            JsonPutValue(sb, v->members[i].value);
        }
        SbPutc(sb, '}');
        break;
    }
}
