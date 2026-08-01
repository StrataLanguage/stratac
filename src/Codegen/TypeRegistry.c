#include "Codegen/TypeRegistry.h"

#include <stdlib.h>
#include <string.h>

void TypeRegistryInit(TypeRegistry* reg)
{
    reg->types = NULL;
    reg->count = 0;
    reg->cap = 0;
}

void TypeRegistryBuild(TypeRegistry* reg, const Module* m)
{
    reg->count = 0;

    for (size_t i = 0; i < m->structs.count; i++)
    {
        StructDecl* sd = (StructDecl*)VecGet((Vec*)&m->structs, i);

        if (reg->count >= reg->cap)
        {
            reg->cap = reg->cap ? reg->cap * 2 : 8;
            reg->types = (StructType*)realloc(reg->types, reg->cap * sizeof(StructType));
        }

        StructType* t = &reg->types[reg->count++];
        t->name = sd->name;
        t->opaque = false;
        t->extendsFrom = NULL;
        t->fields = sd->fields;
    }

    for (size_t i = 0; i < m->handles.count; i++)
    {
        HandleDecl* hd = (HandleDecl*)VecGet((Vec*)&m->handles, i);

        if (reg->count >= reg->cap)
        {
            reg->cap = reg->cap ? reg->cap * 2 : 8;
            reg->types = (StructType*)realloc(reg->types, reg->cap * sizeof(StructType));
        }

        StructType* t = &reg->types[reg->count++];
        t->name = hd->name;
        t->opaque = true;
        t->extendsFrom = hd->extendsName;
        VecInit(&t->fields);
    }
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
