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

    /// Lanes for a SIMD vector (2/4/8).
    int lanes;

    char elemIr[16];
    char ir[32];
} MappedType;

/* Forward declaration — full definition lives in TypeRegistry.h. */
typedef struct TypeRegistry TypeRegistry;

MappedType MapType(const TypeName* t);

/* Resolve a type alias name through the registry, returning the final
   non-alias name. If `name` is not a type alias, returns `name` unchanged.
   Requires the registry; if reg is NULL the name is returned as-is. */
const char* ResolveAliasName(const TypeRegistry* reg, const char* name);

bool IsNumeric(const char* t);

/**
 * @brief Returns 0 if not a SIMD vector. Otherwise, returns the number of lanes for the vector.
 */
int IsSimdVector(const char* t);

// Returns the number of vector components held by the type.
int GetSimdVectorLanes(const char* t);

bool IsScalarTypeName(const char* t);
bool IsFloatType(const char* t);

/* True if `t` is a scalar type or a type alias whose underlying type is scalar.
   Requires the registry to resolve aliases; if reg is NULL, only built-in scalars match. */
bool IsScalarLikeType(const TypeRegistry* reg, const char* t);
