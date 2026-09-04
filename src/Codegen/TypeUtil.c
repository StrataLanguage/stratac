#include "Codegen/TypeUtil.h"
#include "Codegen/TypeRegistry.h"

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

static MappedType MakePrimitive(bool isFloat, bool isUnsigned, int bits, const char* elemIr)
{
    MappedType m = {0};

    m.valid = true;
    m.isFloat = isFloat;
    m.isUnsigned = isUnsigned;
    m.isSimdVector = false;
    m.bits = bits;

    BuildLLVMIrType(&m, 1, elemIr);

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
        return MakePrimitive(false, false, 1, "i1");
    }

    if (strcmp(base, "int") == 0)
    {
        return MakePrimitive(false, false, 32, "i32");
    }

    if (strcmp(base, "uint") == 0)
    {
        return MakePrimitive(false, true, 32, "i32");
    }

    if (strcmp(base, "long") == 0)
    {
        return MakePrimitive(false, false, 64, "i64");
    }

    if (strcmp(base, "ulong") == 0)
    {
        return MakePrimitive(false, true, 64, "i64");
    }

    if (strcmp(base, "sbyte") == 0)
    {
        return MakePrimitive(false, false, 8, "i8");
    }

    if (strcmp(base, "byte") == 0)
    {
        return MakePrimitive(false, true, 8, "i8");
    }

    if (strcmp(base, "short") == 0)
    {
        return MakePrimitive(false, false, 16, "i16");
    }

    if (strcmp(base, "ushort") == 0)
    {
        return MakePrimitive(false, true, 16, "i16");
    }

    if (strcmp(base, "float") == 0)
    {
        return MakePrimitive(true, false, 32, "float");
    }

    if (strcmp(base, "double") == 0)
    {
        return MakePrimitive(true, false, 64, "double");
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

bool IsScalarTypeName(const char* t)
{
    return IsNumeric(t) || strcmp(t, "bool") == 0;
}

bool IsFloatType(const char* t)
{
    return strcmp(t, "double") == 0 || strcmp(t, "float") == 0;
}

const char* ResolveAliasName(const TypeRegistry* reg, const char* name)
{
    if (!reg || !name)
    {
        return name;
    }

    return TypeRegistryResolveAlias(reg, name);
}

bool IsScalarLikeType(const TypeRegistry* reg, const char* t)
{
    if (IsScalarTypeName(t))
    {
        return true;
    }

    if (reg && TypeRegistryIsTypeAlias(reg, t))
    {
        const char* underlying = TypeRegistryResolveAlias(reg, t);
        return IsScalarTypeName(underlying);
    }

    return false;
}
