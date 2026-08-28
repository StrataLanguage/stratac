#include "Codegen/TypeUtil.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static inline void BuildLLVMIrType(MappedType* mappedType, int numLanes, const char* elemIr)
{
    assert(numLanes >= 1);

    strncpy(mappedType->elemIr, elemIr, sizeof(mappedType->elemIr) - 1);

    if (numLanes == 1)
    {
        strncpy(mappedType->ir, elemIr, sizeof(mappedType->ir) - 1);
    }
    else
    {
        // <numLanes x type>
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

    // Keep a separate `lanes` member so existing `vec` logic is unaffected; TODO: unify.
    m.vec = 1;
    m.lanes = lanes;

    // Emit as <numLanes x type> for the backend.
    BuildLLVMIrType(&m, m.lanes, elemIr);

    return m;
}

MappedType MapType(const TypeName* t)
{
    MappedType m = {0};

    if (!t || !t->name || t->name[0] == '\0')
    {
        return m;
    }

    const char* base = t->name;
    char candidate[32] = {0};
    int vec = 1;

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

    if (strcmp(base, "float2") == 0)
    {
        return MakeSimdVector(true, 32, 2, "float");
    }

    if (strcmp(base, "float3") == 0 || strcmp(base, "float4") == 0)
    {
        return MakeSimdVector(true, 32, 4, "float");
    }

    return m;
}

bool IsNumeric(const char* t)
{
    return strcmp(t, "int") == 0 || strcmp(t, "uint") == 0 || strcmp(t, "long") == 0 || strcmp(t, "ulong") == 0
           || strcmp(t, "byte") == 0 || strcmp(t, "sbyte") == 0 || strcmp(t, "short") == 0 || strcmp(t, "ushort") == 0
           || strcmp(t, "float") == 0 || strcmp(t, "double") == 0 || strcmp(t, "bool") == 0;
}

int IsSimdVector(const char* t)
{
    if (strcmp(t, "float2") == 0)
    {
        return 2;
    }
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

int GetSimdVectorLanes(const char* t)
{
    if (strcmp(t, "float2") == 0)
    {
        return 2;
    }
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
