#pragma once

#include "Core/Util.h"

#include <stdint.h>

typedef enum PrimitiveType
{
    /* Non-primitive value */
    PrimNone = 0,

    PrimBool,
    PrimInt,
    PrimUInt,
    PrimLong,
    PrimULong,
    PrimSByte,
    PrimByte,
    PrimShort,
    PrimUShort,
    PrimFloat,
    PrimDouble,
    PrimString,
} PrimitiveType;


/* NOTE: These are prebaked to be guaranteed to match `HashStr64` and ONLY `HashStr64`.
   Do not mix with another FNV1-A algorithm. */

#define PRIM_NAME_BOOL 0x26f69aafbc840517
#define PRIM_NAME_INT 0x268b425134d641ac
#define PRIM_NAME_UINT 0x9c36416143ba191b
#define PRIM_NAME_LONG 0x8fcea42686b8279d
#define PRIM_NAME_ULONG 0x38621b5d7e2c3acc
#define PRIM_NAME_SBYTE 0x9dc0f3e00b77362a
#define PRIM_NAME_BYTE 0x8b89e3af649f4541
#define PRIM_NAME_SHORT 0xe88b6b14a4081493
#define PRIM_NAME_USHORT 0x518ef30ba516e564
#define PRIM_NAME_FLOAT 0x747ea7c3224ac467
#define PRIM_NAME_DOUBLE 0xa83b61858211676e
#define PRIM_NAME_STRING 0x3850125edc81f186

static inline PrimitiveType GetPrimitiveType(const char* name)
{
    const uint64_t nameHash = HashStr64(name);

    switch (nameHash)
    {
    case PRIM_NAME_BOOL:
        return PrimBool;

        /* */
    case PRIM_NAME_INT:
        return PrimInt;
    case PRIM_NAME_UINT:
        return PrimUInt;

         /* */
    case PRIM_NAME_LONG:
        return PrimLong;
    case PRIM_NAME_ULONG:
        return PrimULong;

        /* */
    case PRIM_NAME_SBYTE:
        return PrimSByte;
    case PRIM_NAME_BYTE:
        return PrimByte;

        /* */
    case PRIM_NAME_SHORT:
        return PrimShort;
    case PRIM_NAME_USHORT:
        return PrimUShort;

        /* */
    case PRIM_NAME_FLOAT:
        return PrimFloat;
    case PRIM_NAME_DOUBLE:
        return PrimDouble;
        /* */

    case PRIM_NAME_STRING:
        return PrimString;
    default:;
    }

    return PrimNone;
}
