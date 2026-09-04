#pragma once

#include "AST/AST.h"

#include <stdbool.h>

typedef struct {
    bool valid;
    bool isVoid;
    bool isFloat;
    bool isUnsigned;
    bool isSimdVector;
    int bits;

    // SIMD lane count (2/4), 0 when not a vector.
    int lanes;

    char elemIr[16];
    char ir[32];
} MappedType;

/* Full definition lives in TypeRegistry.h. */
typedef struct TypeRegistry TypeRegistry;
typedef struct Arena Arena;

MappedType MapType(const TypeName* t);

bool IsNumeric(const char* t);

// 0 when not a vector, else the lane count.
int IsSimdVector(const char* t);

// Same as IsSimdVector; kept for call-site readability.
int GetSimdVectorLanes(const char* t);

bool IsScalarTypeName(const char* t);
bool IsFloatType(const char* t);

/* Builtin numeric scalars eligible for `max`/`min` pseudo-properties:
   int/uint/long/ulong/byte/sbyte/short/ushort/float/double (NOT bool).
   True only for the BUILTIN names — never aliases or user types. */
bool IsScalarPseudoType(const char* t);

/* Scalar pseudo-property read (`int.max`, `float.min`): `t` must be a
   builtin scalar pseudo type and `member` must be "max" (every scalar) or
   "min" (float/double only). When valid, fills the constant value: `outInt`
   gets the integer bit pattern (ints), `outFloat` the double (floats),
   `outIsFloat` distinguishes them. Any output may be NULL. */
bool ScalarPseudoConst(const char* t, const char* member, uint64_t* outInt, double* outFloat, bool* outIsFloat);

// Alias-aware scalar check; NULL registry means builtins only.
bool IsScalarLikeType(const TypeRegistry* reg, const char* t);

// True when the (alias-resolved) name is "string".
bool TypeIsString(const TypeRegistry* reg, const char* name);

/* True when `==`/`!=` (and ordering) compares the value directly — scalars,
   handles, enums, and aliases of those compare by value / pointer identity.
   Dynamic arrays are NOT trivially comparable (fat `{ptr, len}` structs;
   equality would need an element-wise compare, which isn't implemented —
   default false, built upon later). Strings are NOT either: `==` is CONTENT
   equality (codegen `strata_str_eq`), never a raw fat-pointer compare. */
bool TypeIsTriviallyComparable(const TypeRegistry* reg, const TypeName* t);

// True when the type owns (box/array/string, or alias of one).
bool TypeIsOwningResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t);
