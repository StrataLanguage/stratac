#include "Json.h"

#include <stdlib.h>

#define JSON_MAX_DEPTH 128

typedef struct {
    Arena* arena;
    const char* text;
    size_t len;
    size_t pos;
    int depth;
} JsonParser;

static JsonValue* ParseValue(JsonParser* p);

static void SkipWhitespace(JsonParser* p)
{
    while (p->pos < p->len)
    {
        char c = p->text[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
            break;
        }
        p->pos++;
    }
}

static bool ConsumeLiteral(JsonParser* p, const char* word)
{
    size_t n = strlen(word);
    if (p->len - p->pos < n || memcmp(p->text + p->pos, word, n) != 0)
    {
        return false;
    }
    p->pos += n;
    return true;
}

static JsonValue* NewValue(JsonParser* p, JsonKind kind, size_t start)
{
    JsonValue* v = (JsonValue*)arena_alloc(p->arena, sizeof(JsonValue));
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    v->raw = STR_N(p->text + start, 0);
    return v;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ParseHex4(JsonParser* p, uint32_t* out)
{
    if (p->len - p->pos < 4)
    {
        return false;
    }

    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
    {
        int d = HexDigit(p->text[p->pos + i]);
        if (d < 0)
        {
            return false;
        }
        value = (value << 4) | (uint32_t)d;
    }

    p->pos += 4;
    *out = value;
    return true;
}

static void PutUtf8(Sb* sb, uint32_t cp)
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

// Parses the string starting at the opening quote into `out` (arena-owned, NUL-terminated).
static bool ParseStringInto(JsonParser* p, Str* out)
{
    p->pos++; // opening quote

    // Fast path: no escapes, so the decoded string is a straight copy.
    size_t start = p->pos;
    while (p->pos < p->len && p->text[p->pos] != '"' && p->text[p->pos] != '\\')
    {
        if ((unsigned char)p->text[p->pos] < 0x20)
        {
            return false;
        }
        p->pos++;
    }

    if (p->pos < p->len && p->text[p->pos] == '"')
    {
        *out = STR_N(arena_strndup(p->arena, p->text + start, p->pos - start), p->pos - start);
        p->pos++;
        return true;
    }

    Sb sb;
    SbInit(&sb);
    SbPutn(&sb, p->text + start, p->pos - start);

    while (p->pos < p->len)
    {
        char c = p->text[p->pos++];

        if (c == '"')
        {
            *out = STR_N(arena_strndup(p->arena, sb.data ? sb.data : "", sb.len), sb.len);
            SbFree(&sb);
            return true;
        }

        if ((unsigned char)c < 0x20)
        {
            break;
        }

        if (c != '\\')
        {
            SbPutc(&sb, c);
            continue;
        }

        if (p->pos >= p->len)
        {
            break;
        }

        char e = p->text[p->pos++];
        switch (e)
        {
        case '"':
        case '\\':
        case '/':
            SbPutc(&sb, e);
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
            uint32_t cp = 0;
            if (!ParseHex4(p, &cp))
            {
                SbFree(&sb);
                return false;
            }

            // A high surrogate followed by `\uDC00..\uDFFF` encodes one astral code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && p->len - p->pos >= 6 && p->text[p->pos] == '\\'
                && p->text[p->pos + 1] == 'u')
            {
                size_t save = p->pos;
                uint32_t low = 0;
                p->pos += 2;
                if (ParseHex4(p, &low) && low >= 0xDC00 && low <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else
                {
                    p->pos = save;
                }
            }

            if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                cp = 0xFFFD; // lone surrogate
            }

            PutUtf8(&sb, cp);
            break;
        }
        default:
            SbFree(&sb);
            return false;
        }
    }

    SbFree(&sb);
    return false;
}

static JsonValue* ParseNumber(JsonParser* p)
{
    size_t start = p->pos;

    if (p->text[p->pos] == '-')
    {
        p->pos++;
    }

    bool digits = false;
    while (p->pos < p->len)
    {
        char c = p->text[p->pos];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
        {
            digits = digits || (c >= '0' && c <= '9');
            p->pos++;
            continue;
        }
        break;
    }

    if (!digits)
    {
        return NULL;
    }

    char* copy = arena_strndup(p->arena, p->text + start, p->pos - start);
    char* end = NULL;

    JsonValue* v = NewValue(p, JsonNumber, start);
    v->number = strtod(copy, &end);

    if (end == copy || *end != '\0')
    {
        return NULL;
    }

    return v;
}

// Grows an arena-backed array by doubling; the old block is left to the arena.
static void* GrowArray(Arena* arena, void* items, size_t count, size_t* cap, size_t elemSize)
{
    if (count < *cap)
    {
        return items;
    }

    size_t newCap = *cap ? *cap * 2 : 4;
    void* grown = arena_alloc(arena, newCap * elemSize);
    if (count)
    {
        memcpy(grown, items, count * elemSize);
    }

    *cap = newCap;
    return grown;
}

static JsonValue* ParseArray(JsonParser* p, size_t start)
{
    p->pos++; // [
    JsonValue* v = NewValue(p, JsonArray, start);
    size_t cap = 0;

    SkipWhitespace(p);
    if (p->pos < p->len && p->text[p->pos] == ']')
    {
        p->pos++;
        return v;
    }

    for (;;)
    {
        JsonValue* item = ParseValue(p);
        if (!item)
        {
            return NULL;
        }

        v->items = (JsonValue**)GrowArray(p->arena, v->items, v->count, &cap, sizeof(JsonValue*));
        v->items[v->count++] = item;

        SkipWhitespace(p);
        if (p->pos >= p->len)
        {
            return NULL;
        }

        char c = p->text[p->pos++];
        if (c == ']')
        {
            return v;
        }
        if (c != ',')
        {
            return NULL;
        }
    }
}

static JsonValue* ParseObject(JsonParser* p, size_t start)
{
    p->pos++; // {
    JsonValue* v = NewValue(p, JsonObject, start);
    size_t itemCap = 0;
    size_t keyCap = 0;

    SkipWhitespace(p);
    if (p->pos < p->len && p->text[p->pos] == '}')
    {
        p->pos++;
        return v;
    }

    for (;;)
    {
        SkipWhitespace(p);
        if (p->pos >= p->len || p->text[p->pos] != '"')
        {
            return NULL;
        }

        Str key;
        if (!ParseStringInto(p, &key))
        {
            return NULL;
        }

        SkipWhitespace(p);
        if (p->pos >= p->len || p->text[p->pos] != ':')
        {
            return NULL;
        }
        p->pos++;

        JsonValue* item = ParseValue(p);
        if (!item)
        {
            return NULL;
        }

        v->items = (JsonValue**)GrowArray(p->arena, v->items, v->count, &itemCap, sizeof(JsonValue*));
        v->keys = (Str*)GrowArray(p->arena, v->keys, v->count, &keyCap, sizeof(Str));
        v->items[v->count] = item;
        v->keys[v->count] = key;
        v->count++;

        SkipWhitespace(p);
        if (p->pos >= p->len)
        {
            return NULL;
        }

        char c = p->text[p->pos++];
        if (c == '}')
        {
            return v;
        }
        if (c != ',')
        {
            return NULL;
        }
    }
}

static JsonValue* ParseValue(JsonParser* p)
{
    SkipWhitespace(p);
    if (p->pos >= p->len)
    {
        return NULL;
    }

    if (++p->depth > JSON_MAX_DEPTH)
    {
        return NULL;
    }

    size_t start = p->pos;
    JsonValue* v = NULL;
    char c = p->text[p->pos];

    switch (c)
    {
    case '{':
        v = ParseObject(p, start);
        break;
    case '[':
        v = ParseArray(p, start);
        break;
    case '"':
    {
        Str s;
        if (ParseStringInto(p, &s))
        {
            v = NewValue(p, JsonString, start);
            v->string = s;
        }
        break;
    }
    case 't':
        if (ConsumeLiteral(p, "true"))
        {
            v = NewValue(p, JsonBool, start);
            v->boolean = true;
        }
        break;
    case 'f':
        if (ConsumeLiteral(p, "false"))
        {
            v = NewValue(p, JsonBool, start);
        }
        break;
    case 'n':
        if (ConsumeLiteral(p, "null"))
        {
            v = NewValue(p, JsonNull, start);
        }
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            v = ParseNumber(p);
        }
        break;
    }

    p->depth--;

    if (v)
    {
        v->raw.len = p->pos - start;
    }

    return v;
}

JsonValue* JsonParse(Arena* arena, const char* text, size_t len)
{
    JsonParser p = {arena, text, len, 0, 0};

    JsonValue* v = ParseValue(&p);
    if (!v)
    {
        return NULL;
    }

    SkipWhitespace(&p);
    return p.pos == p.len ? v : NULL;
}

const JsonValue* JsonGet(const JsonValue* obj, const char* key)
{
    if (!obj || obj->kind != JsonObject)
    {
        return NULL;
    }

    for (size_t i = 0; i < obj->count; i++)
    {
        if (StrEqC(obj->keys[i], key))
        {
            return obj->items[i];
        }
    }

    return NULL;
}

const JsonValue* JsonGetPath(const JsonValue* obj, const char* path)
{
    char key[128];

    while (obj && *path)
    {
        const char* dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof(key))
        {
            return NULL;
        }

        memcpy(key, path, n);
        key[n] = '\0';

        obj = JsonGet(obj, key);
        path += n + (dot ? 1 : 0);
    }

    return obj;
}

const char* JsonGetString(const JsonValue* obj, const char* path)
{
    const JsonValue* v = JsonGetPath(obj, path);
    return v && v->kind == JsonString ? v->string.data : NULL;
}

void JsonWriteString(Sb* sb, const char* s, size_t len)
{
    static const char kHex[] = "0123456789abcdef";

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
                char esc[7] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0xF], '\0'};
                SbPuts(sb, esc);
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
