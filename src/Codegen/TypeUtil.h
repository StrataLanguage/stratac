#pragma once

#include "AST/AST.h"

#include <stdbool.h>

typedef struct {
    bool valid;
    bool isVoid;
    bool isFloat;
    bool isUnsigned;
    int bits;
    int vec;
    char elemIr[16];
    char ir[32];
} MappedType;

static inline bool MappedTypeIsVector(const MappedType* m)
{
    return m->vec > 1;
}

MappedType MapType(const TypeName* t);
