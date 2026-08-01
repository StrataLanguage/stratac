#include "Codegen/TypeUtil.h"

#include <string.h>
#include <stdio.h>

static MappedType MakePrimitive(bool isFloat, bool isUnsigned, int bits, const char* elemIr, int vec)
{
    MappedType m = {0};

    m.valid = true;
    m.isFloat = isFloat;
    m.isUnsigned = isUnsigned;
    m.bits = bits;
    m.vec = vec;
    strncpy(m.elemIr, elemIr, sizeof(m.elemIr) - 1);

    if (vec == 1)
    {
        strncpy(m.ir, elemIr, sizeof(m.ir) - 1);
    }
    else
    {
        snprintf(m.ir, sizeof(m.ir), "<%d x %s>", vec, elemIr);
    }

    return m;
}

MappedType MapType(const TypeName* t)
{
    MappedType m = {0};

    if (!t->name || t->name[0] == '\0')
    {
        return m;
    }

    const char* base = t->name;
    char candidate[32] = {0};
    int vec = 1;

    size_t len = strlen(base);
    size_t split = len;

    while (split > 0 && base[split - 1] >= '0' && base[split - 1] <= '9')
    {
        --split;
    }

    if (split > 0 && split < len)
    {
        int parsedSize = 0;
        for (size_t i = split; i < len; ++i)
        {
            parsedSize = parsedSize * 10 + (base[i] - '0');
        }

        if (parsedSize >= 1 && parsedSize <= 4)
        {
            size_t clen = split < sizeof(candidate) - 1 ? split : sizeof(candidate) - 1;
            memcpy(candidate, base, clen);
            candidate[clen] = '\0';

            if (strcmp(candidate, "float") == 0
                || strcmp(candidate, "int") == 0
                || strcmp(candidate, "uint") == 0
                || strcmp(candidate, "double") == 0
                || strcmp(candidate, "bool") == 0)
            {
                base = candidate;
                vec = parsedSize;
            }
        }
    }

    if (strcmp(base, "void") == 0)
    {
        m.valid = true;
        m.isVoid = true;
        strcpy(m.ir, "void");
        strcpy(m.elemIr, "void");

        return m;
    }

    if (strcmp(base, "bool") == 0)
    {
        return MakePrimitive(false, false, 1, "i1", vec);
    }

    if (strcmp(base, "int") == 0)
    {
        return MakePrimitive(false, false, 32, "i32", vec);
    }

    if (strcmp(base, "uint") == 0)
    {
        return MakePrimitive(false, true, 32, "i32", vec);
    }

    if (strcmp(base, "long") == 0)
    {
        return MakePrimitive(false, false, 64, "i64", vec);
    }

    if (strcmp(base, "ulong") == 0)
    {
        return MakePrimitive(false, true, 64, "i64", vec);
    }

    if (strcmp(base, "float") == 0)
    {
        return MakePrimitive(true, false, 32, "float", vec);
    }

    if (strcmp(base, "double") == 0)
    {
        return MakePrimitive(true, false, 64, "double", vec);
    }

    return m;
}

bool IsNumeric(const char* t)
{
    return strcmp(t, "int") == 0
        || strcmp(t, "uint") == 0
        || strcmp(t, "long") == 0
        || strcmp(t, "ulong") == 0
        || strcmp(t, "float") == 0
        || strcmp(t, "double") == 0
        || strcmp(t, "bool") == 0;
}

bool IsScalarTypeName(const char* t)
{
    return IsNumeric(t) || strcmp(t, "bool") == 0;
}

bool IsFloatType(const char* t)
{
    return strcmp(t, "double") == 0
        || strcmp(t, "float") == 0;
}