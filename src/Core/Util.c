#include "Core/Util.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// -- C string

char* DupString(const char* s)
{
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);

    if (out)
    {
        memcpy(out, s, n + 1);
    }

    return out;
}

// -- Str

Str StrNew(Arena* arena, const char* str, size_t len)
{
    if (!arena)
    {
        STRATA_CRASH("Null arena");
    }

    if (!str)
    {
        return (Str){0};
    }

    char* strDuped = arena_strndup(arena, str, len);
    return (Str){strDuped, len};
}

void StrFree(Arena* arena, Str s)
{
    if (!arena)
    {
        STRATA_CRASH("Null arena");
    }

    if (!s.data)
    {
        return;
    }

    arena_free(arena, s.data);
}

// -- Arena

static void ArenaGrow(Arena* a, size_t need)
{
    size_t chunk_size = a->default_chunk;

    if (chunk_size < need + sizeof(ArenaChunk))
    {
        chunk_size = need + sizeof(ArenaChunk);
    }
    
    chunk_size = (chunk_size + 63) & ~(size_t)63;

    ArenaChunk* c = (ArenaChunk*)malloc(chunk_size);
    if (!c)
    {
        STRATA_OOM();
    }

    c->next = a->head;
    c->size = chunk_size - sizeof(ArenaChunk);

    a->head = c;
    a->ptr = (char*)(c + 1);
    a->end = a->ptr + c->size;
}

void arena_init(Arena* a, size_t initial_chunk)
{
    *a = (Arena){0};
    a->head = NULL;
    a->default_chunk = initial_chunk > 0 ? initial_chunk : (1 << 16);
    a->ptr = NULL;
    a->end = NULL;
}

void arena_free(Arena* a)
{
    if (!a)
    {
        return;
    }

    ArenaChunk* c = a->head;

    while (c)
    {
        ArenaChunk* next = c->next;
        free(c);
        c = next;
    }

    a->head = NULL;
    a->ptr = NULL;
    a->end = NULL;
}

void* arena_alloc_aligned(Arena* a, size_t size, size_t align)
{
    if (align == 0)
    {
        align = 1;
    }

    uintptr_t p = (uintptr_t)a->ptr;
    uintptr_t aligned = (p + align - 1) & ~((uintptr_t)align - 1);
    size_t pad = aligned - p;

    if (a->ptr + pad + size > a->end)
    {
        ArenaGrow(a, size + align);
    }

    p = (uintptr_t)a->ptr;
    aligned = (p + align - 1) & ~((uintptr_t)align - 1);
    pad = aligned - p;

    char* result = a->ptr + pad;
    memset(result, 0, size);

    a->ptr = result + size;

    return result;
}

void* arena_alloc(Arena* a, size_t size)
{
    return arena_alloc_aligned(a, size, sizeof(void*));
}

void* arena_dup(Arena* a, const void* src, size_t size)
{
    void* dst = arena_alloc(a, size);
    memcpy(dst, src, size);

    return dst;
}

char* arena_strdup(Arena* a, const char* s)
{
    size_t n = strlen(s) + 1;
    char* dst = (char*)arena_alloc(a, n);
    memcpy(dst, s, n);

    return dst;
}

char* arena_strndup(Arena* a, const char* s, size_t n)
{
    char* dst = (char*)arena_alloc(a, n + 1);
    memcpy(dst, s, n);
    dst[n] = '\0';

    return dst;
}

char* arena_vformat(Arena* a, const char* fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);

    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (n < 0)
    {
        va_end(args2);

        return NULL;
    }

    char* buf = (char*)arena_alloc(a, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, args2);
    va_end(args2);

    return buf;
}

char* arena_format(Arena* a, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char* result = arena_vformat(a, fmt, args);
    va_end(args);

    return result;
}

static __thread Arena g_scratch;

Arena* scratch_arena(void)
{
    if (!g_scratch.head)
    {
        arena_init(&g_scratch, 0);
    }
    return &g_scratch;
}

void scratch_reset(void)
{
    if (g_scratch.head)
    {
        arena_free(&g_scratch);
        arena_init(&g_scratch, 0);
    }
}

// -- Vec

void VecReserve(Vec* v, size_t n)
{
    if (n <= v->cap)
    {
        return;
    }

    size_t newcap = v->cap ? v->cap * 2 : 8;
    
    while (newcap < n)
    {
        newcap *= 2;
    }
    
    v->items = (void**)realloc(v->items, newcap * sizeof(void*));
    if (!v->items)
    {
        STRATA_OOM();
    }

    v->cap = newcap;
}

void VecPush(Vec* v, void* item)
{
    if (v->count >= v->cap)
    {
        VecReserve(v, v->count + 1);
    }

    v->items[v->count++] = item;
}

void* VecPop(Vec* v)
{
    if (v->count == 0)
    {
        return NULL;
    }
    return v->items[--v->count];
}

// -- StrMap

static uint64_t StrMapHash(const char* s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void StrMapResize(StrMap* m, size_t newcap)
{
    const char** newkeys = (const char**)calloc(newcap, sizeof(const char*));
    void** newvals = (void**)calloc(newcap, sizeof(void*));

    if (!newkeys || !newvals)
    {
        STRATA_OOM();
    }

    for (size_t i = 0; i < m->cap; i++)
    {
        if (m->keys[i])
        {
            uint64_t h = StrMapHash(m->keys[i]) & (newcap - 1);

            while (newkeys[h])
            {
                h = (h + 1) & (newcap - 1);
            }

            newkeys[h] = m->keys[i];
            newvals[h] = m->values[i];
        }
    }

    free(m->keys);
    free(m->values);

    m->keys = newkeys;
    m->values = newvals;
    m->cap = newcap;
}

void StrMapPut(StrMap* m, const char* key, void* value)
{
    if (m->cap == 0)
    {
        StrMapResize(m, 16);
    }

    if ((m->count + 1) * 4 >= m->cap * 3)
    {
        StrMapResize(m, m->cap * 2);
    }

    uint64_t h = StrMapHash(key) & (m->cap - 1);

    while (m->keys[h])
    {
        if (strcmp(m->keys[h], key) == 0)
        {
            m->values[h] = value;

            return;
        }

        h = (h + 1) & (m->cap - 1);
    }

    m->keys[h] = key;
    m->values[h] = value;
    m->count++;
}

void* StrMapGet(const StrMap* m, const char* key)
{
    if (m->cap == 0)
    {
        return NULL;
    }

    uint64_t h = StrMapHash(key) & (m->cap - 1);

    while (m->keys[h])
    {
        if (strcmp(m->keys[h], key) == 0)
        {
            return m->values[h];
        }

        h = (h + 1) & (m->cap - 1);
    }

    return NULL;
}

void StrMapFree(StrMap* m)
{
    free(m->keys);
    free(m->values);
    StrMapInit(m);
}

void StrMapClear(StrMap* m)
{
    if (m->cap > 0)
    {
        for (size_t i = 0; i < m->cap; i++)
        {
            m->keys[i] = NULL;
        }

        m->count = 0;
    }
}

static void SbEnsure(Sb* sb, size_t extra)
{
    if (sb->len + extra <= sb->cap)
    {
        return;
    }

    size_t newcap = sb->cap ? sb->cap * 2 : 64;
    while (newcap < sb->len + extra)
    {
        newcap *= 2;
    }

    sb->data = (char*)realloc(sb->data, newcap);
    if (!sb->data)
    {
        STRATA_OOM();
    }
    
    sb->cap = newcap;
}

// -- StringBuilder

void SbPutc(Sb* sb, char c)
{
    SbEnsure(sb, 1);

    sb->data[sb->len++] = c;
}

void SbPuts(Sb* sb, const char* s)
{
    size_t n = strlen(s);
    if (n == 0)
    {
        return;
    }
    
    SbEnsure(sb, n);
    memcpy(sb->data + sb->len, s, n);
    
    sb->len += n;
}

void SbPutn(Sb* sb, const char* s, size_t n)
{
    if (n == 0)
    {
        return;
    }

    SbEnsure(sb, n);
    memcpy(sb->data + sb->len, s, n);
    
    sb->len += n;
}

void SbPutr(Sb* sb, char c, size_t repeat)
{
    if (repeat == 0)
    {
        return;
    }

    SbEnsure(sb, repeat);
    memset(sb->data + sb->len, c, repeat);
    
    sb->len += repeat;
}

void SbPrintf(Sb* sb, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n > 0)
    {
        SbEnsure(sb, (size_t)n + 1);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, args2);
        sb->len += (size_t)n;
    }
    va_end(args2);
}

char* SbFinish(Sb* sb, Arena* arena)
{
    char* result = (char*)arena_alloc(arena, sb->len + 1);
    
    if (sb->data)
    {
        memcpy(result, sb->data, sb->len);
    }

    result[sb->len] = '\0';
    free(sb->data);

    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;

    return result;
}

// -- Misc

size_t UpperBound(const uint32_t* arr, size_t count, uint32_t val)
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