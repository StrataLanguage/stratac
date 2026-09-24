#ifndef STRATA_TYPES_H
#define STRATA_TYPES_H

/*
 * Strata type metadata: a self-describing blob that lists a module's `@component`
 * structs, plus every struct and enum those components use, so a host can register
 * them without the compiler being present at runtime.
 *
 * The same bytes come out of every path:
 *   - JIT:        strataJitGetTypeMetadata
 *   - AOT:        a read-only data symbol named `<prefix>__strata_types` in the object
 *                 (prefix from strataSetSymbolPrefix, empty by default)
 *   - types-only: strataCompileTypeMetadata (parse + check, no code generation)
 *
 * This header only reads the blob. It needs no Strata library, so hosts can include it in
 * builds that ship without the compiler.
 *
 * Layout: little-endian, fixed-width integers, every reference an offset from the blob start
 * or an index into a table. There are no pointers, so the blob is relocation-free.
 *
 *   header (headerSize bytes)
 *   struct table       structCount * structRecordSize
 *   field table        fieldCount * fieldRecordSize
 *   attribute table    attributeCount * attributeRecordSize
 *   enum table         enumCount * enumRecordSize
 *   enum value table   enumValueCount * enumValueRecordSize
 *   string table       NUL-terminated UTF-8; offset 0 is the empty string
 *   data section       each struct's default value (struct size bytes, 16-byte aligned)
 *
 * Compatibility: record sizes are stored in the header and readers step by them, so newer
 * compilers may append fields to records (and kinds to StrataTypeKind) without breaking older
 * readers. `version` changes only for incompatible layouts.
 *
 * Structs are listed in dependency order: a struct only uses structs listed before it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STRATA_TYPES_MAGIC 0x54525453u /* "STRT" */
#define STRATA_TYPES_VERSION 1u
#define STRATA_TYPES_SYMBOL "__strata_types"

    /* Values are part of the format: only ever appended to. */
    typedef enum StrataTypeKind
    {
        STRATA_TYPE_INVALID = 0,
        STRATA_TYPE_BOOL = 1,
        STRATA_TYPE_I8 = 2,
        STRATA_TYPE_U8 = 3,
        STRATA_TYPE_I16 = 4,
        STRATA_TYPE_U16 = 5,
        STRATA_TYPE_I32 = 6,
        STRATA_TYPE_U32 = 7,
        STRATA_TYPE_I64 = 8,
        STRATA_TYPE_U64 = 9,
        STRATA_TYPE_F32 = 10,
        STRATA_TYPE_F64 = 11,
        STRATA_TYPE_FLOAT2 = 12, /* 8 bytes, 8-aligned */
        STRATA_TYPE_FLOAT3 = 13, /* 16 bytes, 16-aligned; lanes x, y, z then 4 bytes of padding */
        STRATA_TYPE_FLOAT4 = 14, /* 16 bytes, 16-aligned */
        STRATA_TYPE_STRUCT = 15, /* typeIndex = struct index */
        STRATA_TYPE_ENUM = 16,   /* typeIndex = enum index; stored as the enum's underlying kind */
        STRATA_TYPE_HANDLE = 17, /* opaque host pointer; typeName = the handle type */
    } StrataTypeKind;

    /* StrataTypesStruct::flags */
    enum
    {
        STRATA_TYPES_STRUCT_COMPONENT = 1u << 0,
    };

#define STRATA_TYPES_NO_INDEX 0xFFFFFFFFu

    /* Minimum record sizes this reader understands (the blob may use larger ones). */
#define STRATA_TYPES_HEADER_SIZE 96u
#define STRATA_TYPES_STRUCT_RECORD_SIZE 48u
#define STRATA_TYPES_FIELD_RECORD_SIZE 32u
#define STRATA_TYPES_ATTRIBUTE_RECORD_SIZE 8u
#define STRATA_TYPES_ENUM_RECORD_SIZE 16u
#define STRATA_TYPES_ENUM_VALUE_RECORD_SIZE 16u

    typedef struct StrataTypesTable
    {
        uint32_t count;
        uint32_t recordSize;
        uint32_t offset;
    } StrataTypesTable;

    typedef struct StrataTypes
    {
        const uint8_t* data;
        uint32_t size;
        uint32_t version;
        uint32_t pointerSize;
        StrataTypesTable structs;
        StrataTypesTable fields;
        StrataTypesTable attributes;
        StrataTypesTable enums;
        StrataTypesTable enumValues;
        uint32_t stringOffset;
        uint32_t stringSize;
        uint32_t dataOffset;
        uint32_t dataSize;
    } StrataTypes;

    typedef struct StrataTypesStruct
    {
        const char* name;
        const char* moduleName;
        uint32_t size;
        uint32_t alignment;
        uint32_t flags;
        uint32_t firstField;
        uint32_t fieldCount;
        uint32_t firstAttribute;
        uint32_t attributeCount;
        const void* defaultValue; /* `size` bytes: the struct with its field defaults applied */
        uint64_t layoutHash;      /* changes when the layout changes (not when only defaults do) */
    } StrataTypesStruct;

    typedef struct StrataTypesField
    {
        const char* name;
        uint32_t offset;
        uint32_t size;        /* total, including every element of an array */
        StrataTypeKind kind;  /* element kind for fixed-size arrays */
        uint32_t arrayLength; /* 0 = not an array; nested arrays are flattened */
        uint32_t typeIndex;   /* struct / enum index, or STRATA_TYPES_NO_INDEX */
        const char* typeName; /* struct, enum or handle type name; "" for scalars */
    } StrataTypesField;

    typedef struct StrataTypesEnum
    {
        const char* name;
        StrataTypeKind underlyingKind;
        uint32_t firstValue;
        uint32_t valueCount;
    } StrataTypesEnum;

    typedef struct StrataTypesEnumValue
    {
        const char* name;
        uint64_t value; /* bit pattern in the underlying type (sign-extended for signed kinds) */
    } StrataTypesEnumValue;

    static inline uint32_t strataTypesReadU32_(const uint8_t* p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static inline uint64_t strataTypesReadU64_(const uint8_t* p)
    {
        return (uint64_t)strataTypesReadU32_(p) | ((uint64_t)strataTypesReadU32_(p + 4) << 32);
    }

    static inline int strataTypesTableValid_(const StrataTypesTable* table, uint32_t minimumRecordSize, uint32_t totalSize)
    {
        if (table->count == 0)
        {
            return 1;
        }

        if (table->recordSize < minimumRecordSize || table->offset > totalSize)
        {
            return 0;
        }

        return (uint64_t)table->count * table->recordSize <= (uint64_t)(totalSize - table->offset);
    }

    static inline void strataTypesReadTable_(const uint8_t* header, uint32_t at, StrataTypesTable* out)
    {
        out->count = strataTypesReadU32_(header + at);
        out->recordSize = strataTypesReadU32_(header + at + 4);
        out->offset = strataTypesReadU32_(header + at + 8);
    }

    /* Validates the blob and fills `out`. Returns 1 on success, 0 when the data is truncated,
       corrupt, or from an incompatible version. */
    static inline int strataTypesOpen(StrataTypes* out, const void* data, size_t size)
    {
        const uint8_t* bytes = (const uint8_t*)data;

        memset(out, 0, sizeof(*out));

        if (!bytes || size < STRATA_TYPES_HEADER_SIZE || size > 0xFFFFFFFFu)
        {
            return 0;
        }

        if (strataTypesReadU32_(bytes) != STRATA_TYPES_MAGIC)
        {
            return 0;
        }

        uint32_t version = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8);
        uint32_t headerSize = (uint32_t)bytes[6] | ((uint32_t)bytes[7] << 8);
        uint32_t totalSize = strataTypesReadU32_(bytes + 8);

        if (version != STRATA_TYPES_VERSION || headerSize < STRATA_TYPES_HEADER_SIZE || totalSize > size
            || headerSize > totalSize)
        {
            return 0;
        }

        out->data = bytes;
        out->size = totalSize;
        out->version = version;
        out->pointerSize = bytes[12];

        strataTypesReadTable_(bytes, 16, &out->structs);
        strataTypesReadTable_(bytes, 28, &out->fields);
        strataTypesReadTable_(bytes, 40, &out->attributes);
        strataTypesReadTable_(bytes, 52, &out->enums);
        strataTypesReadTable_(bytes, 64, &out->enumValues);

        out->stringOffset = strataTypesReadU32_(bytes + 76);
        out->stringSize = strataTypesReadU32_(bytes + 80);
        out->dataOffset = strataTypesReadU32_(bytes + 84);
        out->dataSize = strataTypesReadU32_(bytes + 88);

        int valid = strataTypesTableValid_(&out->structs, STRATA_TYPES_STRUCT_RECORD_SIZE, totalSize)
                    && strataTypesTableValid_(&out->fields, STRATA_TYPES_FIELD_RECORD_SIZE, totalSize)
                    && strataTypesTableValid_(&out->attributes, STRATA_TYPES_ATTRIBUTE_RECORD_SIZE, totalSize)
                    && strataTypesTableValid_(&out->enums, STRATA_TYPES_ENUM_RECORD_SIZE, totalSize)
                    && strataTypesTableValid_(&out->enumValues, STRATA_TYPES_ENUM_VALUE_RECORD_SIZE, totalSize)
                    && out->stringSize > 0 && out->stringOffset <= totalSize
                    && out->stringSize <= totalSize - out->stringOffset
                    && out->data[out->stringOffset + out->stringSize - 1] == '\0' && out->dataOffset <= totalSize
                    && out->dataSize <= totalSize - out->dataOffset;

        if (!valid)
        {
            memset(out, 0, sizeof(*out));
            return 0;
        }

        return 1;
    }

    /* A string by table offset; "" for out-of-range offsets (the table ends in a NUL, so every
       in-range offset is terminated). */
    static inline const char* strataTypesString_(const StrataTypes* types, uint32_t offset)
    {
        if (offset >= types->stringSize)
        {
            return "";
        }

        return (const char*)(types->data + types->stringOffset + offset);
    }

    static inline const uint8_t* strataTypesRecord_(const StrataTypes* types, const StrataTypesTable* table,
                                                    uint32_t index)
    {
        if (index >= table->count)
        {
            return NULL;
        }

        return types->data + table->offset + (size_t)index * table->recordSize;
    }

    static inline uint32_t strataTypesStructCount(const StrataTypes* types)
    {
        return types->structs.count;
    }

    static inline int strataTypesGetStruct(const StrataTypes* types, uint32_t index, StrataTypesStruct* out)
    {
        const uint8_t* record = strataTypesRecord_(types, &types->structs, index);

        if (!record)
        {
            return 0;
        }

        out->name = strataTypesString_(types, strataTypesReadU32_(record));
        out->moduleName = strataTypesString_(types, strataTypesReadU32_(record + 4));
        out->size = strataTypesReadU32_(record + 8);
        out->alignment = strataTypesReadU32_(record + 12);
        out->flags = strataTypesReadU32_(record + 16);
        out->firstField = strataTypesReadU32_(record + 20);
        out->fieldCount = strataTypesReadU32_(record + 24);
        out->firstAttribute = strataTypesReadU32_(record + 28);
        out->attributeCount = strataTypesReadU32_(record + 32);

        uint32_t defaultOffset = strataTypesReadU32_(record + 36);
        out->layoutHash = strataTypesReadU64_(record + 40);

        if ((uint64_t)out->firstField + out->fieldCount > types->fields.count
            || (uint64_t)out->firstAttribute + out->attributeCount > types->attributes.count
            || defaultOffset > types->dataSize || out->size > types->dataSize - defaultOffset)
        {
            return 0;
        }

        out->defaultValue = types->data + types->dataOffset + defaultOffset;

        return 1;
    }

    static inline int strataTypesGetField(const StrataTypes* types, const StrataTypesStruct* owner, uint32_t fieldIndex,
                                          StrataTypesField* out)
    {
        if (fieldIndex >= owner->fieldCount)
        {
            return 0;
        }

        const uint8_t* record = strataTypesRecord_(types, &types->fields, owner->firstField + fieldIndex);

        if (!record)
        {
            return 0;
        }

        out->name = strataTypesString_(types, strataTypesReadU32_(record));
        out->offset = strataTypesReadU32_(record + 4);
        out->size = strataTypesReadU32_(record + 8);
        out->kind = (StrataTypeKind)record[12];
        out->arrayLength = strataTypesReadU32_(record + 16);
        out->typeIndex = strataTypesReadU32_(record + 20);
        out->typeName = strataTypesString_(types, strataTypesReadU32_(record + 24));

        return (uint64_t)out->offset + out->size <= owner->size;
    }

    static inline int strataTypesGetAttribute(const StrataTypes* types, const StrataTypesStruct* owner,
                                              uint32_t attributeIndex, const char** outName)
    {
        if (attributeIndex >= owner->attributeCount)
        {
            return 0;
        }

        const uint8_t* record = strataTypesRecord_(types, &types->attributes, owner->firstAttribute + attributeIndex);

        if (!record)
        {
            return 0;
        }

        *outName = strataTypesString_(types, strataTypesReadU32_(record));

        return 1;
    }

    static inline int strataTypesHasAttribute(const StrataTypes* types, const StrataTypesStruct* owner,
                                              const char* name)
    {
        for (uint32_t i = 0; i < owner->attributeCount; i++)
        {
            const char* attributeName = NULL;

            if (strataTypesGetAttribute(types, owner, i, &attributeName) && strcmp(attributeName, name) == 0)
            {
                return 1;
            }
        }

        return 0;
    }

    static inline uint32_t strataTypesEnumCount(const StrataTypes* types)
    {
        return types->enums.count;
    }

    static inline int strataTypesGetEnum(const StrataTypes* types, uint32_t index, StrataTypesEnum* out)
    {
        const uint8_t* record = strataTypesRecord_(types, &types->enums, index);

        if (!record)
        {
            return 0;
        }

        out->name = strataTypesString_(types, strataTypesReadU32_(record));
        out->underlyingKind = (StrataTypeKind)record[4];
        out->firstValue = strataTypesReadU32_(record + 8);
        out->valueCount = strataTypesReadU32_(record + 12);

        return (uint64_t)out->firstValue + out->valueCount <= types->enumValues.count;
    }

    static inline int strataTypesGetEnumValue(const StrataTypes* types, const StrataTypesEnum* owner,
                                              uint32_t valueIndex, StrataTypesEnumValue* out)
    {
        if (valueIndex >= owner->valueCount)
        {
            return 0;
        }

        const uint8_t* record = strataTypesRecord_(types, &types->enumValues, owner->firstValue + valueIndex);

        if (!record)
        {
            return 0;
        }

        out->name = strataTypesString_(types, strataTypesReadU32_(record));
        out->value = strataTypesReadU64_(record + 8);

        return 1;
    }

    /* Byte size of one element of `kind` (0 for STRUCT, whose size is the struct's own). */
    static inline uint32_t strataTypesKindSize(StrataTypeKind kind)
    {
        switch (kind)
        {
        case STRATA_TYPE_BOOL:
        case STRATA_TYPE_I8:
        case STRATA_TYPE_U8:
            return 1;
        case STRATA_TYPE_I16:
        case STRATA_TYPE_U16:
            return 2;
        case STRATA_TYPE_I32:
        case STRATA_TYPE_U32:
        case STRATA_TYPE_F32:
            return 4;
        case STRATA_TYPE_I64:
        case STRATA_TYPE_U64:
        case STRATA_TYPE_F64:
        case STRATA_TYPE_FLOAT2:
        case STRATA_TYPE_HANDLE:
            return 8;
        case STRATA_TYPE_FLOAT3:
        case STRATA_TYPE_FLOAT4:
            return 16;
        default:
            return 0;
        }
    }

#ifdef __cplusplus
}
#endif

#endif /* STRATA_TYPES_H */
