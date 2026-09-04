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

// Alias-aware scalar check; NULL registry means builtins only.
bool IsScalarLikeType(const TypeRegistry* reg, const char* t);

// True when the (alias-resolved) name is "string".
bool TypeIsString(const TypeRegistry* reg, const char* name);

// True when the type owns (box/array/string, or alias of one).
bool TypeIsOwningResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t);
