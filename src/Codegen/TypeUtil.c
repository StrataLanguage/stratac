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

    if (t->primitiveType == PrimVoid)
    {
        m.valid = true;
        m.isVoid = true;
        strcpy(m.ir, "void");
        strcpy(m.elemIr, "void");

        return m;
    }

    // (name, isFloat, isUnsigned, bits, IR)
    static const struct
    {
        PrimitiveType primitive;
        // const char* name;
        bool isFloat;
        bool isUnsigned;
        int bits;
        const char* ir;
    } kPrims[] = {
        {PrimBool,   false, false, 1,  "i1"    },
        {PrimInt,    false, false, 32, "i32"   },
        {PrimUInt,   false, true,  32, "i32"   },
        {PrimLong,   false, false, 64, "i64"   },
        {PrimULong,  false, true,  64, "i64"   },
        {PrimSByte,  false, false, 8,  "i8"    },
        {PrimByte,   false, true,  8,  "i8"    },
        {PrimShort,  false, false, 16, "i16"   },
        {PrimUShort, false, true,  16, "i16"   },
        {PrimFloat,  true,  false, 32, "float" },
        {PrimDouble, true,  false, 64, "double"},
    };

    for (size_t i = 0; i < sizeof(kPrims) / sizeof(kPrims[0]); i++)
    {
        if (t->primitiveType == kPrims[i].primitive)
        {
            return MakePrimitive(kPrims[i].isFloat, kPrims[i].isUnsigned, kPrims[i].bits, kPrims[i].ir);
        }
    }

    if (t->primitiveType == PrimFloat2)
    {
        return MakeSimdVector(true, 32, 2, "float");
    }

    if (t->primitiveType == PrimFloat3 || t->primitiveType == PrimFloat4)
    {
        return MakeSimdVector(true, 32, 4, "float");
    }

    return m;
}

bool IsNumeric(PrimitiveType primType)
{
    switch (primType)
    {
    case PrimInt:
    case PrimUInt:
    case PrimLong:
    case PrimULong:
    case PrimSByte:
    case PrimByte:
    case PrimShort:
    case PrimUShort:
    case PrimFloat:
    case PrimDouble:
    case PrimBool:
        return true;
    default:;
    }

    return false;
}

bool IsNameNumeric(const char* t)
{
    return t && IsNumeric(GetPrimitiveType(t));
}

int IsSimdVector(const PrimitiveType prim)
{
    switch (prim)
    {
    case PrimFloat2:
        return 2;
    case PrimFloat3:
        return 3;
    case PrimFloat4:
        return 4;
    default:;
    }

    return 0;
}

int IsNameSimdVector(const char* t)
{
    return t ? IsSimdVector(GetPrimitiveType(t)) : 0;
}

bool IsScalarType(const PrimitiveType prim)
{
    return IsNumeric(prim);
}

bool IsNameScalarType(const char* t)
{
    return t && IsScalarType(GetPrimitiveType(t));
}

bool IsNameFloatType(const char* t)
{
    return t && IsFloatType(GetPrimitiveType(t));
}

bool IsFloatType(const PrimitiveType prim)
{
    return (prim == PrimFloat || prim == PrimDouble);
}

bool IsNameScalarPseudoType(const char* t)
{
    return t && IsScalarPseudoType(GetPrimitiveType(t));
}

bool IsScalarPseudoType(PrimitiveType prim)
{
    return IsScalarType(prim) && !(prim == PrimBool);
}

bool IsStringType(PrimitiveType prim)
{
    return (prim == PrimString);
}

bool IsCStringType(PrimitiveType prim)
{
    return (prim == PrimCString);
}

bool ScalarPseudoConst(PrimitiveType primitive, const char* member, uint64_t* outInt, double* outFloat,
                       bool* outIsFloat)
{
    if (!IsScalarPseudoType(primitive))
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
    if (isMin && !IsFloatType(primitive))
    {
        return false;
    }

    static const struct
    {
        PrimitiveType primitive;
        // const char* name;
        uint64_t maxInt;
    } kIntMax[] = {
        {PrimInt,    0x7FFFFFFFULL        },
        {PrimUInt,   0xFFFFFFFFULL        },
        {PrimLong,   0x7FFFFFFFFFFFFFFFULL},
        {PrimULong,  0xFFFFFFFFFFFFFFFFULL},
        {PrimByte,   0xFFULL              },
        {PrimSByte,  0x7FULL              },
        {PrimShort,  0x7FFFULL            },
        {PrimUShort, 0xFFFFULL            },
    };

    if (!IsFloatType(primitive))
    {
        for (size_t i = 0; i < sizeof(kIntMax) / sizeof(kIntMax[0]); i++)
        {
            if (primitive == kIntMax[i].primitive)
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
    if (primitive == PrimDouble)
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
    return leaf && GetPrimitiveType(leaf) == PrimString;
}

bool TypeIsCString(const TypeRegistry* reg, const char* name)
{
    if (!name)
    {
        return false;
    }

    const char* leaf = reg ? TypeRegistryResolveAlias(reg, name) : name;
    return leaf && GetPrimitiveType(leaf) == PrimCString;
}

bool TypeIsComparableAggregate(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    /* `T[]?` compares like the array it wraps (the fat {ptr, len, cap}; null ptr
       with len 0 is the canonical empty). */
    if (t->isOptional && t->inner && TypeNameIsArray(t->inner))
    {
        return true;
    }

    if (TypeNameIsArray(t))
    {
        return true;
    }

    /* Defined structs (plain or extern). Handles/forward decls are opaque,
       aliases/enums are scalar-like — all excluded. */
    const StructType* st = reg ? TypeRegistryFind(reg, t->name) : NULL;

    return st && !st->isTypeAlias && !st->isEnum && !st->opaque && !st->incomplete;
}

bool TypeIsTriviallyComparable(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    /* Arrays and structs are aggregates: they take the structural-equality
       path (TypeIsComparableAggregate), never this value-compare fallback. */
    if (TypeIsComparableAggregate(reg, t))
    {
        return false;
    }

    /* Strings compare by CONTENT (codegen strata_str_eq with a length
       fast-out), never by raw fat-pointer comparison. `cstring` likewise
       compares by content (NUL-terminated, lengths via strlen). */
    if (TypeIsString(reg, t->name) || TypeIsCString(reg, t->name))
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

    if (leaf && GetPrimitiveType(leaf) == PrimString)
    {
        return true;
    }

    TypeName parsed = TypeNameParse(arena, leaf);
    return TypeNameIsOwning(&parsed);
}

bool TypeIsOwningValueResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (TypeIsOwningResolved(reg, arena, t))
    {
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(reg, t->name);

    return leaf && TypeRegistryIsOwningStruct(reg, leaf);
}

bool IsScalarLikeType(const TypeRegistry* reg, const char* t)
{
    if (IsNameScalarType(t))
    {
        return true;
    }

    if (reg && TypeRegistryIsTypeAlias(reg, t))
    {
        const char* underlying = TypeRegistryResolveAlias(reg, t);
        return IsNameScalarType(underlying);
    }

    return false;
}
