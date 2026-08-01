#include "Codegen/TypeRegistry.h"

#include <stdlib.h>
#include <string.h>
void TypeRegistryInit(TypeRegistry* reg)
{
    *reg = (TypeRegistry){0};
}

void TypeRegistryFree(TypeRegistry* reg)
{
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

    return t;
}

void TypeRegistryBuild(TypeRegistry* reg, const Module* m)
{
    reg->count = 0;

    /* Complete struct definitions first: a full body wins over a forward decl,
       and the first definition of a given name is kept. */
    for (size_t i = 0; i < m->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet((Vec*)&m->structs, i);

        if (sd->incomplete)
        {
            continue;
        }

        if (TypeRegistryFindIndex(reg, sd->name) >= 0)
        {
            continue;
        }

        StructType* t = TypeRegistryAdd(reg, sd->name);
        t->opaque = false;
        t->incomplete = false;
        t->fields = sd->fields;
    }

    /* Forward declarations for structs that never received a body: they stay
       incomplete (usable as an opaque reference, but not instantiable). */
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

    /* Fixpoint: a struct is owning if it has a box<T> field or an owning field. */
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

                if (IsBoxTypeName(f->type.name))
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
}

bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);

    return t && !t->opaque && t->owning;
}

const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name)
{
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

    while (t && t->opaque && t->extendsFrom)
    {
        if (strcmp(t->extendsFrom, base) == 0)
        {
            return true;
        }

        t = TypeRegistryFind(reg, t->extendsFrom);
    }

    return false;
}

bool IsHandleType(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque && !t->incomplete;
}

bool IsBoxTypeName(const char* name)
{
    if (!name)
    {
        return false;
    }

    size_t len = strlen(name);

    return len > 5 && strncmp(name, "box<", 4) == 0 && name[len - 1] == '>';
}

bool BoxInnerTypeName(const char* name, char* buf, size_t cap)
{
    if (!IsBoxTypeName(name) || !buf || cap == 0)
    {
        return false;
    }

    size_t len = strlen(name);
    size_t innerLen = len - 5; /* strip "box<" (4) and trailing ">" (1) */

    if (innerLen >= cap)
    {
        innerLen = cap - 1;
    }

    memcpy(buf, name + 4, innerLen);
    buf[innerLen] = '\0';

    return true;
}