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
    int vec;

    /// Lanes for a SIMD vector (2/4/8).
    int lanes;

    char elemIr[16];
    char ir[32];
} MappedType;

static inline bool MappedTypeIsVector(const MappedType* m)
{
    return m->vec > 1;
}

MappedType MapType(const TypeName* t);

bool IsNumeric(const char* t);

/**
 * @brief Returns 0 if not a SIMD vector. Otherwise, returns the number of lanes for the vector.
 */
int IsSimdVector(const char* t);

// Returns the number of vector components held by the type.
int GetSimdVectorLanes(const char* t);

bool IsScalarTypeName(const char* t);
bool IsFloatType(const char* t);
