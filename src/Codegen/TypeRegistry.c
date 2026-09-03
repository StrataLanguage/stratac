#include "Codegen/TypeRegistry.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ComputeAllLayouts(TypeRegistry* reg);

void TypeRegistryInit(TypeRegistry* reg)
{
    *reg = (TypeRegistry){0};
}

void TypeRegistryFree(TypeRegistry* reg)
{
    for (size_t i = 0; i < reg->count; i++)
    {
        free(reg->types[i].fieldOffsets);
        free(reg->types[i].physicalIndex);
        free(reg->types[i].pads);
        free(reg->types[i].layoutError);
    }

    free(reg->types);
    TypeRegistryInit(reg);
}

static int TypeRegistryFindIndex(const TypeRegistry* reg, const char* name)
{
    for (size_t i = 0; i < reg->count; i++)
    {
        if (strcmp(reg->types[i].name, name) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static StructType* TypeRegistryAdd(TypeRegistry* reg, const char* name)
{
    if (reg->count >= reg->cap)
    {
        reg->cap = reg->cap ? reg->cap * 2 : 8;
        reg->types = (StructType*)realloc(reg->types, reg->cap * sizeof(StructType));
    }

    StructType* t = &reg->types[reg->count++];
    t->name = name;
    t->opaque = false;
    t->incomplete = false;
    t->owning = false;
    t->extendsFrom = NULL;
    VecInit(&t->fields);
    t->isExtern = false;
    t->isTypeAlias = false;
    t->underlyingType = NULL;
    t->hasLayout = false;
    t->packedLayout = false;
    t->sizeBytes = 0;
    t->alignBytes = 0;
    t->fieldOffsets = NULL;
    t->physicalIndex = NULL;
    t->pads = NULL;
    t->padCount = 0;
    t->physicalCount = 0;
    t->layoutError = NULL;

    return t;
}

static void SetLayoutError(StructType* t, const char* fmt, ...)
{
    if (t->layoutError)
    {
        return;
    }

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    t->layoutError = strdup(buf);
}

static bool FieldsIdentical(const Vec* a, const Vec* b)
{
    if (a->count != b->count)
    {
        return false;
    }

    for (size_t i = 0; i < a->count; i++)
    {
        FieldDecl* fa = (FieldDecl*)VecGet((Vec*)a, i);
        FieldDecl* fb = (FieldDecl*)VecGet((Vec*)b, i);

        if (strcmp(fa->name, fb->name) != 0 || strcmp(fa->type.name, fb->type.name) != 0 || fa->offset != fb->offset)
        {
            return false;
        }
    }

    return true;
}

void TypeRegistryRegisterAliases(TypeRegistry* reg, const Module* m)
{
    /* Type aliases — registered early so other types can reference them. */
    for (size_t i = 0; i < m->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet((Vec*)&m->structs, i);

        if (!sd->isTypeAlias)
        {
            continue;
        }

        if (TypeRegistryFindIndex(reg, sd->name) >= 0)
        {
            continue;
        }

        StructType* t = TypeRegistryAdd(reg, sd->name);
        t->isTypeAlias = true;
        t->underlyingType = sd->underlyingType;
        t->opaque = false;
        t->incomplete = false;
    }
}

void TypeRegistryBuild(TypeRegistry* reg, const Module* m)
{
    reg->count = 0;

    TypeRegistryRegisterAliases(reg, m);

    /* Structs */
    for (size_t i = 0; i < m->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet((Vec*)&m->structs, i);

        if (sd->incomplete)
        {
            continue;
        }

        int existing = TypeRegistryFindIndex(reg, sd->name);

        if (existing >= 0)
        {
            /* Extern structs mirror host layouts, so a divergent duplicate
               is a hard error (plain structs keep first-wins behavior). */
            StructType* ex = &reg->types[existing];

            if (ex->isExtern || sd->isExtern)
            {
                if (ex->isExtern != sd->isExtern)
                {
                    SetLayoutError(ex, "struct '%s' is declared both as extern and non-extern", sd->name);
                }
                else if (!FieldsIdentical(&ex->fields, &sd->fields))
                {
                    SetLayoutError(ex, "conflicting redeclaration of extern struct '%s'", sd->name);
                }
            }

            continue;
        }

        StructType* t = TypeRegistryAdd(reg, sd->name);
        t->opaque = false;
        t->incomplete = false;
        t->fields = sd->fields;
        t->isExtern = sd->isExtern;
    }

    /* Forward declarations for structs (incomplete types) */
    for (size_t i = 0; i < m->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet((Vec*)&m->structs, i);

        if (!sd->incomplete)
        {
            continue;
        }

        if (TypeRegistryFindIndex(reg, sd->name) >= 0)
        {
            continue;
        }

        StructType* t = TypeRegistryAdd(reg, sd->name);
        t->opaque = true;
        t->incomplete = true;
    }

    for (size_t i = 0; i < m->handles.count; i++)
    {
        HandleDecl* hd = (HandleDecl*)VecGet((Vec*)&m->handles, i);

        if (TypeRegistryFindIndex(reg, hd->name) >= 0)
        {
            continue;
        }

        StructType* t = TypeRegistryAdd(reg, hd->name);
        t->opaque = true;
        t->incomplete = false;
        t->extendsFrom = hd->extendsName;
    }

    /* a struct is owning if it has a ^T field or an owning field. */
    bool changed = true;

    while (changed)
    {
        changed = false;

        for (size_t i = 0; i < reg->count; i++)
        {
            StructType* t = &reg->types[i];

            if (t->opaque || t->owning)
            {
                continue;
            }

            for (size_t j = 0; j < t->fields.count; j++)
            {
                FieldDecl* f = (FieldDecl*)VecGet(&t->fields, j);

                if (TypeNameIsOwning(&f->type))
                {
                    t->owning = true;
                    changed = true;
                    break;
                }

                const StructType* ft = TypeRegistryFind(reg, f->type.name);

                if (ft && ft->owning)
                {
                    t->owning = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    ComputeAllLayouts(reg);
}

//--

typedef struct
{
    long size;
    long align;
} SizeAlign;

/* Transient per-struct layout state: 0 = new, 1 = computing, 2 = done,
   3 = failed. Stored in a parallel array owned by ComputeAllLayouts. */

static bool ComputeStructLayout(TypeRegistry* reg, unsigned char* state, size_t idx);

static bool ScalarSizeAlign(const char* name, SizeAlign* out)
{
    if (strcmp(name, "bool") == 0 || strcmp(name, "byte") == 0 || strcmp(name, "sbyte") == 0)
    {
        out->size = 1;
        out->align = 1;
        return true;
    }

    if (strcmp(name, "short") == 0 || strcmp(name, "ushort") == 0)
    {
        out->size = 2;
        out->align = 2;
        return true;
    }

    if (strcmp(name, "int") == 0 || strcmp(name, "uint") == 0 || strcmp(name, "float") == 0)
    {
        out->size = 4;
        out->align = 4;
        return true;
    }

    if (strcmp(name, "long") == 0 || strcmp(name, "ulong") == 0 || strcmp(name, "double") == 0)
    {
        out->size = 8;
        out->align = 8;
        return true;
    }

    if (strcmp(name, "float2") == 0)
    {
        out->size = 8;
        out->align = 8;
        return true;
    }

    /* float3, float4 are 16 byte aligned */
    if (strcmp(name, "float3") == 0 || strcmp(name, "float4") == 0)
    {
        out->size = 16;
        out->align = 16;
        return true;
    }

    return false;
}

static bool FieldSizeAlign(TypeRegistry* reg, unsigned char* state, const TypeName* t, SizeAlign* out)
{
    if (t->isBox || t->isOptional)
    {
        /* Boxes and optionals are pointer-sized slots. */
        out->size = 8;
        out->align = 8;
        return true;
    }

    if (t->isArray)
    {
        if (t->length < 0)
        {
            /* Dynamic array is fat pointer: {ptr, i64}. */
            out->size = 16;
            out->align = 8;
            return true;
        }

        SizeAlign elem;

        if (!FieldSizeAlign(reg, state, t->elem, &elem))
        {
            return false;
        }

        out->size = elem.size * t->length;
        out->align = elem.align;
        return true;
    }

    if (strcmp(t->name, "string") == 0)
    {
        out->size = 8;
        out->align = 8;
        return true;
    }

    if (ScalarSizeAlign(t->name, out))
    {
        return true;
    }

    int idx = TypeRegistryFindIndex(reg, t->name);

    if (idx < 0)
    {
        return false;
    }

    StructType* st = &reg->types[idx];

    if (st->opaque)
    {
        out->size = 8;
        out->align = 8;
        return true;
    }

    if (state[idx] == 2)
    {
        out->size = st->sizeBytes;
        out->align = st->alignBytes;
        return st->alignBytes > 0;
    }

    if (state[idx] != 0)
    {
        /* computing (by-value cycle) or previously failed */
        return false;
    }

    if (!ComputeStructLayout(reg, state, (size_t)idx))
    {
        return false;
    }

    out->size = st->sizeBytes;
    out->align = st->alignBytes;
    return true;
}

static long AlignUp(long v, long a)
{
    return (v + a - 1) / a * a;
}

static bool ComputeStructLayout(TypeRegistry* reg, unsigned char* state, size_t idx)
{
    StructType* st = &reg->types[idx];

    if (st->opaque || st->incomplete || st->fields.count == 0)
    {
        st->sizeBytes = 0;
        st->alignBytes = 1;
        state[idx] = 2;
        return true;
    }

    state[idx] = 1;

    size_t n = st->fields.count;
    long* offsets = (long*)malloc(n * sizeof(long));
    int* physical = (int*)malloc(n * sizeof(int));
    StructPad* pads = (StructPad*)malloc((n + 1) * sizeof(StructPad));
    size_t padCount = 0;
    bool ok = true;

    long cursor = 0;
    long maxAlign = 1;
    bool packed = false;
    int nextPhysical = 0;

    for (size_t i = 0; i < n && ok; i++)
    {
        FieldDecl* f = (FieldDecl*)VecGet(&st->fields, i);
        SizeAlign sa;

        if (!FieldSizeAlign(reg, state, &f->type, &sa))
        {
            SetLayoutError(st,
                           "field '%s' has type '%s' with no computable size (unknown, incomplete, or by-value cycle)",
                           f->name, f->type.name);
            ok = false;
            break;
        }

        long off;

        if (f->offset >= 0)
        {
            packed = true;
            off = f->offset;

            if (off < cursor)
            {
                SetLayoutError(st,
                               "fieldoffset(%ld) for field '%s' overlaps the previous field (data ends at byte %ld)",
                               off, f->name, cursor);
                ok = false;
                break;
            }
        }
        else
        {
            off = AlignUp(cursor, sa.align);
        }

        if (off > cursor)
        {
            pads[padCount].beforeField = i;
            pads[padCount].bytes = off - cursor;
            padCount++;
            nextPhysical++;
        }

        offsets[i] = off;
        physical[i] = nextPhysical;
        nextPhysical++;

        if (sa.align > maxAlign)
        {
            maxAlign = sa.align;
        }

        cursor = off + sa.size;
    }

    if (ok)
    {
        if (packed)
        {
            /* Explicit offsets: pads encode the full layout, so backends emit
               packed and the size is exact. */
            st->sizeBytes = cursor;
            st->alignBytes = 1;
        }
        else
        {
            /* Natural layout: backend padding yields the same offsets, so drop
               the bookkeeping pads and use identity member indices. */
            st->sizeBytes = AlignUp(cursor, maxAlign);
            st->alignBytes = maxAlign;
            padCount = 0;

            for (size_t i = 0; i < n; i++)
            {
                physical[i] = (int)i;
            }
        }

        st->packedLayout = packed;
        st->fieldOffsets = offsets;
        st->physicalIndex = physical;
        st->pads = pads;
        st->padCount = padCount;
        st->physicalCount = n + padCount;
        st->hasLayout = true;
        state[idx] = 2;
    }
    else
    {
        free(offsets);
        free(physical);
        free(pads);
        state[idx] = 3;
    }

    return ok;
}

void ComputeAllLayouts(TypeRegistry* reg)
{
    unsigned char* state = (unsigned char*)calloc(reg->count ? reg->count : 1, 1);

    for (size_t i = 0; i < reg->count; i++)
    {
        if (state[i] == 0)
        {
            ComputeStructLayout(reg, state, i);
        }
    }

    free(state);
}

//--

bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);

    return t && !t->opaque && t->owning;
}

bool TypeRegistryIsTypeAlias(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->isTypeAlias;
}

const char* TypeRegistryGetUnderlyingType(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return (t && t->isTypeAlias) ? t->underlyingType : NULL;
}

const char* TypeRegistryResolveAlias(const TypeRegistry* reg, const char* name)
{
    size_t depth = 0;

    while (name)
    {
        const StructType* t = TypeRegistryFind(reg, name);
        if (!t || !t->isTypeAlias)
        {
            return name;
        }

        name = t->underlyingType;

        if (++depth > reg->count)
        {
            return name;
        }
    }

    return name;
}

const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name)
{
    if (!reg || !name)
    {
        return NULL;
    }

    for (size_t i = 0; i < reg->count; i++)
    {
        if (strcmp(reg->types[i].name, name) == 0)
        {
            return &reg->types[i];
        }
    }

    return NULL;
}

bool TypeRegistryIsUserType(const TypeRegistry* reg, const char* name)
{
    return TypeRegistryFind(reg, name) != NULL;
}

bool TypeRegistryIsOpaque(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque;
}

int TypeRegistryFieldIndex(const TypeRegistry* reg, const char* structName, const char* field)
{
    const StructType* t = TypeRegistryFind(reg, structName);

    if (!t)
    {
        return -1;
    }

    for (size_t i = 0; i < t->fields.count; i++)
    {
        FieldDecl* fd = (FieldDecl*)VecGet((Vec*)&t->fields, i);

        if (strcmp(fd->name, field) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base)
{
    const StructType* t = TypeRegistryFind(reg, derived);
    size_t depth = 0;

    while (t && t->opaque && t->extendsFrom)
    {
        if (strcmp(t->extendsFrom, base) == 0)
        {
            return true;
        }

        t = TypeRegistryFind(reg, t->extendsFrom);

        /* Guard against circular `extends` chains (e.g. A extends B; B extends A). */
        if (++depth > reg->count)
        {
            return false;
        }
    }

    return false;
}

bool IsHandleType(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque && !t->incomplete;
}

const Node* MovableBoxSourceNode(const Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((const CastExpr*)n)->operand;
    }

    return IsLValueNode(n) ? n : NULL;
}

bool IsLValueNode(const Node* n)
{
    return n && (n->kind == NodeIdent || n->kind == NodeMember || n->kind == NodeIndex);
}
