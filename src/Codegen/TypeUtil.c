#include "Codegen/TypeUtil.h"
#include "Codegen/TypeRegistry.h"
#include "Core/Util.h"

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

    // (name, isFloat, isUnsigned, bits, IR)
    static const struct {
        const char* name;
        bool isFloat;
        bool isUnsigned;
        int bits;
        const char* ir;
    } kPrims[] = {
        {"bool", false, false, 1, "i1"},   {"int", false, false, 32, "i32"},
        {"uint", false, true, 32, "i32"},  {"long", false, false, 64, "i64"},
        {"ulong", false, true, 64, "i64"}, {"sbyte", false, false, 8, "i8"},
        {"byte", false, true, 8, "i8"},    {"short", false, false, 16, "i16"},
        {"ushort", false, true, 16, "i16"}, {"float", true, false, 32, "float"},
        {"double", true, false, 64, "double"},
    };

    for (size_t i = 0; i < sizeof(kPrims) / sizeof(kPrims[0]); i++)
    {
        if (strcmp(base, kPrims[i].name) == 0)
        {
            return MakePrimitive(kPrims[i].isFloat, kPrims[i].isUnsigned, kPrims[i].bits, kPrims[i].ir);
        }
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

int IsSimdVector(const char* t)
{
    return GetSimdVectorLanes(t);
}

bool IsScalarTypeName(const char* t)
{
    return IsNumeric(t);
}

bool IsFloatType(const char* t)
{
    return strcmp(t, "double") == 0 || strcmp(t, "float") == 0;
}

bool IsScalarPseudoType(const char* t)
{
    return IsScalarTypeName(t) && strcmp(t, "bool") != 0;
}

bool ScalarPseudoConst(const char* t, const char* member, uint64_t* outInt, double* outFloat, bool* outIsFloat)
{
    if (!IsScalarPseudoType(t))
    {
        return false;
    }

    bool isMax = strcmp(member, "max") == 0;
    bool isMin = strcmp(member, "min") == 0;

    if (!isMax && !isMin)
    {
        return false;
    }

    /* `min` is a float-only property (FLT_MIN/DBL_MIN); integers just get
       `max` (mirroring the C limit macros). */
    if (isMin && !IsFloatType(t))
    {
        return false;
    }

    static const struct {
        const char* name;
        uint64_t maxInt;
    } kIntMax[] = {
        {"int", 0x7FFFFFFFULL},   {"uint", 0xFFFFFFFFULL},
        {"long", 0x7FFFFFFFFFFFFFFFULL}, {"ulong", 0xFFFFFFFFFFFFFFFFULL},
        {"byte", 0xFFULL},        {"sbyte", 0x7FULL},
        {"short", 0x7FFFULL},     {"ushort", 0xFFFFULL},
    };

    if (!IsFloatType(t))
    {
        for (size_t i = 0; i < sizeof(kIntMax) / sizeof(kIntMax[0]); i++)
        {
            if (strcmp(t, kIntMax[i].name) == 0)
            {
                if (outInt)
                {
                    *outInt = kIntMax[i].maxInt;
                }
                if (outIsFloat)
                {
                    *outIsFloat = false;
                }
                return true;
            }
        }

        return false;
    }

    /* float/double: FLT_MAX/FLT_MIN, DBL_MAX/DBL_MIN. */
    double value;
    if (strcmp(t, "double") == 0)
    {
        value = isMax ? 1.7976931348623157e+308 : 2.2250738585072014e-308;
    }
    else
    {
        value = isMax ? 3.4028234663852886e+38 : 1.1754943508222875e-38;
    }

    if (outFloat)
    {
        *outFloat = value;
    }
    if (outIsFloat)
    {
        *outIsFloat = true;
    }
    return true;
}

bool TypeIsString(const TypeRegistry* reg, const char* name)
{
    if (!name)
    {
        return false;
    }

    const char* leaf = reg ? TypeRegistryResolveAlias(reg, name) : name;
    return leaf && strcmp(leaf, "string") == 0;
}

bool TypeIsTriviallyComparable(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    /* Arrays (T[] / T[N]) are aggregates: no element-wise comparison exists
       yet, so they are never trivially comparable. Default false — the
       element-wise path can be built on this later. */
    if (TypeNameIsArray(t))
    {
        return false;
    }

    /* Strings compare by CONTENT (codegen strata_str_eq with a length
       fast-out), never by raw fat-pointer comparison. */
    if (TypeIsString(reg, t->name))
    {
        return false;
    }

    return true;
}

bool TypeIsOwningResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t)
{
    if (!t || !t->name || TypeNameIsOwning(t))
    {
        return TypeNameIsOwning(t);
    }

    if (!reg)
    {
        return false;
    }

    const char* leaf = TypeRegistryResolveAlias(reg, t->name);

    if (!leaf || strcmp(leaf, t->name) == 0)
    {
        return false;
    }

    if (strcmp(leaf, "string") == 0)
    {
        return true;
    }

    TypeName parsed = TypeNameParse(arena, leaf);
    return TypeNameIsOwning(&parsed);
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
