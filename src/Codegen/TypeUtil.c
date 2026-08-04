#include "Codegen/TypeUtil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static inline void BuildLLVMIrType(MappedType* mappedType, int numLanes, const char* elemIr)
{
    assert(numLanes >= 1);

    strncpy(mappedType->elemIr, elemIr, sizeof(mappedType->elemIr) - 1);

    // Single scalar value
    if (numLanes == 1)
    {
        strncpy(mappedType->ir, elemIr, sizeof(mappedType->ir) - 1);
    }
    // Vector
    else
    {
        // Build the LLVM vector string (e.g <4 x float>)
        snprintf(mappedType->ir, sizeof(mappedType->ir), "<%d x %s>", numLanes, elemIr);
    }
}

static MappedType MakePrimitive(bool isFloat, bool isUnsigned, int bits, const char* elemIr, int vec)
{
    MappedType m = {0};

    m.valid = true;
    m.isFloat = isFloat;
    m.isUnsigned = isUnsigned;
    m.isSimdVector = false;
    m.bits = bits;
    m.vec = vec;

    BuildLLVMIrType(&m, m.vec, elemIr);

    return m;
}

static MappedType MakeSimdVector(bool isFloat, int bits, int lanes, const char* elemIr)
{
    assert(bits >= 32);

    MappedType m = {0};

    m.valid = true;
    m.isFloat = isFloat;
    m.isUnsigned = false;
    m.isSimdVector = true;
    m.bits = bits;

    // Using a new `lanes` member here instead of `vec` as i want to avoid breaking current functionality with vectors.
    // TODO: unify to one member and replace existing vector stuff with SIMD vectors.
    m.vec = 1;
    m.lanes = lanes;

    // Use `vec.lanes` here as we want the LLVM backend to use <numLanes x type>
    BuildLLVMIrType(&m, m.lanes, elemIr);

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

    // size_t len = strlen(base);
    // size_t split = len;

    // while (split > 0 && base[split - 1] >= '0' && base[split - 1] <= '9')
    // {
    //     --split;
    // }

    // if (split > 0 && split < len)
    // {
    //     int parsedSize = 0;
    //     for (size_t i = split; i < len; ++i)
    //     {
    //         parsedSize = parsedSize * 10 + (base[i] - '0');
    //     }

    //     if (parsedSize >= 1 && parsedSize <= 4)
    //     {
    //         size_t clen = split < sizeof(candidate) - 1 ? split : sizeof(candidate) - 1;
    //         memcpy(candidate, base, clen);
    //         candidate[clen] = '\0';

    //         if (strcmp(candidate, "float") == 0 || strcmp(candidate, "int") == 0 || strcmp(candidate, "uint") == 0
    //             || strcmp(candidate, "double") == 0 || strcmp(candidate, "bool") == 0)
    //         {
    //             base = candidate;
    //             vec = parsedSize;
    //         }
    //     }
    // }

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

    if (strcmp(base, "sbyte") == 0)
    {
        return MakePrimitive(false, false, 8, "i8", vec);
    }

    if (strcmp(base, "byte") == 0)
    {
        return MakePrimitive(false, true, 8, "i8", vec);
    }

    if (strcmp(base, "short") == 0)
    {
        return MakePrimitive(false, false, 16, "i16", vec);
    }

    if (strcmp(base, "ushort") == 0)
    {
        return MakePrimitive(false, true, 16, "i16", vec);
    }

    if (strcmp(base, "float") == 0)
    {
        return MakePrimitive(true, false, 32, "float", vec);
    }

    if (strcmp(base, "double") == 0)
    {
        return MakePrimitive(true, false, 64, "double", vec);
    }

    if (strcmp(base, "float3") == 0)
    {
        // Note that float3's still require 4 components
        return MakeSimdVector(true, 32, 4, "float");
    }

    return m;
}

bool IsNumeric(const char* t)
{
    return strcmp(t, "int") == 0       /* */
           || strcmp(t, "uint") == 0   /* */
           || strcmp(t, "long") == 0   /* */
           || strcmp(t, "ulong") == 0  /* */
           || strcmp(t, "byte") == 0   /* */
           || strcmp(t, "sbyte") == 0  /* */
           || strcmp(t, "short") == 0  /* */
           || strcmp(t, "ushort") == 0 /* */
           || strcmp(t, "float") == 0  /* */
           || strcmp(t, "double") == 0 /* */
           || strcmp(t, "bool") == 0;
}

bool IsSimdVector(const char* t)
{
    return strcmp(t, "float3") == 0 || strcmp(t, "float4") == 0;
}

int GetSimdVectorLanes(const char* t)
{
    if (strcmp(t, "float3") == 0)
    {
        return 3;
    }
    if (strcmp(t, "float4") == 0)
    {
        return 4;
    }

    return 0;
}

bool IsScalarTypeName(const char* t)
{
    return IsNumeric(t) || strcmp(t, "bool") == 0;
}

bool IsFloatType(const char* t)
{
    return strcmp(t, "double") == 0 || strcmp(t, "float") == 0;
}
