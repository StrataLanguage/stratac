#include "Codegen/TypeMetadata.h"

#include "Codegen/TypeUtil.h"
#include "strata/strata_types.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint8_t* data;
    size_t size;
    size_t cap;
} ByteBuffer;

static void BufferReserve(ByteBuffer* buffer, size_t extra)
{
    if (buffer->size + extra <= buffer->cap)
    {
        return;
    }

    size_t cap = buffer->cap ? buffer->cap : 256;

    while (cap < buffer->size + extra)
    {
        cap *= 2;
    }

    buffer->data = (uint8_t*)realloc(buffer->data, cap);
    buffer->cap = cap;
}

static size_t BufferAppend(ByteBuffer* buffer, const void* bytes, size_t count)
{
    BufferReserve(buffer, count);

    size_t offset = buffer->size;

    if (bytes)
    {
        memcpy(buffer->data + offset, bytes, count);
    }
    else
    {
        memset(buffer->data + offset, 0, count);
    }

    buffer->size += count;

    return offset;
}

static void BufferAlign(ByteBuffer* buffer, size_t alignment)
{
    size_t padding = (alignment - buffer->size % alignment) % alignment;

    BufferAppend(buffer, NULL, padding);
}

static void BufferFree(ByteBuffer* buffer)
{
    free(buffer->data);
    *buffer = (ByteBuffer){0};
}

static void PutU16(uint8_t* at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void PutU32(uint8_t* at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void PutU64(uint8_t* at, uint64_t value)
{
    PutU32(at, (uint32_t)value);
    PutU32(at + 4, (uint32_t)(value >> 32));
}

static void PutF32(uint8_t* at, double value)
{
    float narrowed = (float)value;
    uint32_t bits;
    memcpy(&bits, &narrowed, sizeof(bits));
    PutU32(at, bits);
}

static void PutF64(uint8_t* at, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    PutU64(at, bits);
}

#define FNV_OFFSET 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t HashBytes(uint64_t hash, const void* bytes, size_t count)
{
    const uint8_t* at = (const uint8_t*)bytes;

    for (size_t i = 0; i < count; i++)
    {
        hash ^= at[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

static uint64_t HashU64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8];
    PutU64(bytes, value);

    return HashBytes(hash, bytes, sizeof(bytes));
}

static uint64_t HashString(uint64_t hash, const char* text)
{
    return HashBytes(hash, text ? text : "", strlen(text ? text : "") + 1);
}

typedef struct
{
    const StructType* type;
    uint32_t nameString;
    uint32_t moduleNameString;
    uint32_t firstField;
    uint32_t firstAttribute;
    uint32_t defaultOffset;
    uint64_t layoutHash;
} StructEntry;

typedef struct
{
    const EnumDecl* decl;
    StrataTypeKind underlyingKind;
    uint32_t nameString;
    uint32_t firstValue;
} EnumEntry;

typedef struct
{
    StrataTypeKind kind;
    uint32_t arrayLength;
    uint32_t elementSize;
    const StructType* structType;
    const char* typeName;
} FieldShape;

typedef struct
{
    const TypeRegistry* reg;
    const Module* mod;
    Arena scratch;
    Vec structs;  // StructEntry*
    Vec visiting; // const StructType*
    Vec enums;    // EnumEntry*
    ByteBuffer strings;
    ByteBuffer data;
    ByteBuffer fieldRecords;
    ByteBuffer attributeRecords;
    ByteBuffer enumValueRecords;
    uint32_t fieldCount;
    uint32_t attributeCount;
    uint32_t enumValueCount;
} Writer;

static uint32_t InternString(Writer* w, const char* text)
{
    if (!text || !text[0])
    {
        return 0;
    }

    size_t length = strlen(text);

    /* Linear scan: metadata holds a handful of names, not thousands. */
    for (size_t offset = 1; offset < w->strings.size;)
    {
        const char* existing = (const char*)w->strings.data + offset;
        size_t existingLength = strlen(existing);

        if (existingLength == length && memcmp(existing, text, length) == 0)
        {
            return (uint32_t)offset;
        }

        offset += existingLength + 1;
    }

    return (uint32_t)BufferAppend(&w->strings, text, length + 1);
}

static StrataTypeKind KindForPrimitive(PrimitiveType primitive)
{
    switch (primitive)
    {
    case PrimBool:
        return STRATA_TYPE_BOOL;
    case PrimSByte:
        return STRATA_TYPE_I8;
    case PrimByte:
        return STRATA_TYPE_U8;
    case PrimShort:
        return STRATA_TYPE_I16;
    case PrimUShort:
        return STRATA_TYPE_U16;
    case PrimInt:
        return STRATA_TYPE_I32;
    case PrimUInt:
        return STRATA_TYPE_U32;
    case PrimLong:
        return STRATA_TYPE_I64;
    case PrimULong:
        return STRATA_TYPE_U64;
    case PrimFloat:
        return STRATA_TYPE_F32;
    case PrimDouble:
        return STRATA_TYPE_F64;
    case PrimFloat2:
        return STRATA_TYPE_FLOAT2;
    case PrimFloat3:
        return STRATA_TYPE_FLOAT3;
    case PrimFloat4:
        return STRATA_TYPE_FLOAT4;
    default:
        return STRATA_TYPE_INVALID;
    }
}

static const EnumDecl* FindEnumDecl(const Module* mod, const char* name)
{
    for (size_t i = 0; i < mod->enums.count; i++)
    {
        const EnumDecl* decl = (const EnumDecl*)VecGet(&mod->enums, i);

        if (strcmp(decl->name, name) == 0)
        {
            return decl;
        }
    }

    return NULL;
}

static uint32_t EnumIndex(Writer* w, const char* name)
{
    for (size_t i = 0; i < w->enums.count; i++)
    {
        const EnumEntry* entry = (const EnumEntry*)VecGet(&w->enums, i);

        if (strcmp(entry->decl->name, name) == 0)
        {
            return (uint32_t)i;
        }
    }

    return STRATA_TYPES_NO_INDEX;
}

static uint32_t StructIndex(Writer* w, const StructType* type)
{
    for (size_t i = 0; i < w->structs.count; i++)
    {
        if (((const StructEntry*)VecGet(&w->structs, i))->type == type)
        {
            return (uint32_t)i;
        }
    }

    return STRATA_TYPES_NO_INDEX;
}

/* What a field's type looks like to the host. False for types the format can't describe
   (owning types, which components reject). */
static bool ResolveFieldShape(Writer* w, const TypeName* type, FieldShape* out)
{
    *out = (FieldShape){STRATA_TYPE_INVALID, 0, 0, NULL, NULL};

    uint64_t length = 1;
    bool isArray = false;
    const TypeName* current = type;

    for (size_t depth = 0; current && depth <= w->reg->count + 8; depth++)
    {
        while (current && current->isArray && current->length >= 0)
        {
            length *= (uint64_t)current->length;
            isArray = true;
            current = current->elem;
        }

        if (!current || !current->name || current->isArray || current->isBox || current->isOptional
            || length > 0xFFFFFFFFull)
        {
            return false;
        }

        StrataTypeKind primitiveKind = KindForPrimitive(GetPrimitiveType(current->name));

        if (primitiveKind != STRATA_TYPE_INVALID)
        {
            out->kind = primitiveKind;
            out->elementSize = strataTypesKindSize(primitiveKind);
            break;
        }

        const StructType* registered = TypeRegistryFind(w->reg, current->name);

        if (!registered)
        {
            return false;
        }

        if (registered->isEnum)
        {
            StrataTypeKind underlyingKind = KindForPrimitive(GetPrimitiveType(registered->underlyingType));

            out->kind = STRATA_TYPE_ENUM;
            out->elementSize = strataTypesKindSize(underlyingKind);
            out->typeName = registered->name;
            break;
        }

        if (registered->isTypeAlias)
        {
            TypeName* underlying = (TypeName*)arena_alloc(&w->scratch, sizeof(TypeName));
            *underlying = TypeNameParse(&w->scratch, registered->underlyingType);
            current = underlying;
            continue;
        }

        if (registered->opaque)
        {
            if (!IsHandleType(w->reg, registered->name))
            {
                return false;
            }

            out->kind = STRATA_TYPE_HANDLE;
            out->elementSize = 8;
            out->typeName = registered->name;
            break;
        }

        if (!registered->hasLayout)
        {
            return false;
        }

        out->kind = STRATA_TYPE_STRUCT;
        out->elementSize = (uint32_t)registered->sizeBytes;
        out->structType = registered;
        out->typeName = registered->name;
        break;
    }

    if (out->kind == STRATA_TYPE_INVALID)
    {
        return false;
    }

    out->arrayLength = isArray ? (uint32_t)length : 0;

    return true;
}

static void AddEnum(Writer* w, const char* name)
{
    if (EnumIndex(w, name) != STRATA_TYPES_NO_INDEX)
    {
        return;
    }

    const EnumDecl* decl = FindEnumDecl(w->mod, name);

    if (!decl)
    {
        return;
    }

    EnumEntry* entry = (EnumEntry*)arena_alloc(&w->scratch, sizeof(EnumEntry));
    entry->decl = decl;
    entry->underlyingKind = KindForPrimitive(GetPrimitiveType(decl->underlyingType ? decl->underlyingType : "int"));
    VecPush(&w->enums, entry);
}

/* Depth-first, so every struct is listed after the structs it holds by value. */
static bool VisitStruct(Writer* w, const StructType* type)
{
    if (StructIndex(w, type) != STRATA_TYPES_NO_INDEX)
    {
        return true;
    }

    for (size_t i = 0; i < w->visiting.count; i++)
    {
        if (VecGet(&w->visiting, i) == type)
        {
            return false; /* by-value cycle: already a layout error */
        }
    }

    if (!type->hasLayout)
    {
        return false;
    }

    VecPush(&w->visiting, (void*)type);

    bool ok = true;

    for (size_t i = 0; i < type->fields.count && ok; i++)
    {
        const FieldDecl* field = (const FieldDecl*)VecGet(&type->fields, i);
        FieldShape shape;

        if (!ResolveFieldShape(w, &field->type, &shape))
        {
            continue; /* written as STRATA_TYPE_INVALID */
        }

        if (shape.kind == STRATA_TYPE_STRUCT)
        {
            ok = VisitStruct(w, shape.structType);
        }
        else if (shape.kind == STRATA_TYPE_ENUM)
        {
            AddEnum(w, shape.typeName);
        }
    }

    VecPop(&w->visiting);

    if (ok)
    {
        StructEntry* entry = (StructEntry*)arena_alloc(&w->scratch, sizeof(StructEntry));
        entry->type = type;
        VecPush(&w->structs, entry);
    }

    return ok;
}

static StrataTypeKind StorageKind(Writer* w, const FieldShape* shape)
{
    if (shape->kind != STRATA_TYPE_ENUM)
    {
        return shape->kind;
    }

    uint32_t index = EnumIndex(w, shape->typeName);

    return index == STRATA_TYPES_NO_INDEX ? STRATA_TYPE_I32 : ((const EnumEntry*)VecGet(&w->enums, index))->underlyingKind;
}

static void WriteDefaultValue(uint8_t* at, StrataTypeKind kind, const FieldDefaultValue* value)
{
    uint64_t integer = (uint64_t)value->intValue;

    switch (kind)
    {
    case STRATA_TYPE_BOOL:
    case STRATA_TYPE_I8:
    case STRATA_TYPE_U8:
        at[0] = (uint8_t)integer;
        break;
    case STRATA_TYPE_I16:
    case STRATA_TYPE_U16:
        PutU16(at, (uint32_t)integer);
        break;
    case STRATA_TYPE_I32:
    case STRATA_TYPE_U32:
        PutU32(at, (uint32_t)integer);
        break;
    case STRATA_TYPE_I64:
    case STRATA_TYPE_U64:
        PutU64(at, integer);
        break;
    case STRATA_TYPE_F32:
        PutF32(at, value->floatLanes[0]);
        break;
    case STRATA_TYPE_F64:
        PutF64(at, value->floatLanes[0]);
        break;
    case STRATA_TYPE_FLOAT2:
    case STRATA_TYPE_FLOAT3:
    case STRATA_TYPE_FLOAT4:
        for (unsigned lane = 0; lane < value->laneCount && lane < 4; lane++)
        {
            PutF32(at + lane * 4, value->floatLanes[lane]);
        }
        break;
    default:
        break;
    }
}

static void WriteStruct(Writer* w, StructEntry* entry)
{
    const StructType* type = entry->type;
    size_t size = (size_t)type->sizeBytes;
    uint8_t* defaults = (uint8_t*)calloc(size ? size : 1, 1);

    uint64_t hash = FNV_OFFSET;
    hash = HashU64(hash, (uint64_t)type->sizeBytes);
    hash = HashU64(hash, (uint64_t)type->alignBytes);

    entry->firstField = w->fieldCount;

    for (size_t i = 0; i < type->fields.count; i++)
    {
        const FieldDecl* field = (const FieldDecl*)VecGet(&type->fields, i);
        FieldShape shape;
        bool described = ResolveFieldShape(w, &field->type, &shape);

        uint32_t offset = (uint32_t)type->fieldOffsets[i];
        uint32_t elementCount = shape.arrayLength ? shape.arrayLength : 1;
        uint32_t fieldSize = described ? shape.elementSize * elementCount : 0;
        uint32_t typeIndex = STRATA_TYPES_NO_INDEX;

        if (described && shape.kind == STRATA_TYPE_STRUCT)
        {
            typeIndex = StructIndex(w, shape.structType);
        }
        else if (described && shape.kind == STRATA_TYPE_ENUM)
        {
            typeIndex = EnumIndex(w, shape.typeName);
        }

        uint8_t record[STRATA_TYPES_FIELD_RECORD_SIZE] = {0};
        PutU32(record, InternString(w, field->name));
        PutU32(record + 4, offset);
        PutU32(record + 8, fieldSize);
        record[12] = (uint8_t)shape.kind;
        PutU32(record + 16, shape.arrayLength);
        PutU32(record + 20, typeIndex);
        PutU32(record + 24, InternString(w, shape.typeName));
        BufferAppend(&w->fieldRecords, record, sizeof(record));
        w->fieldCount++;

        hash = HashString(hash, field->name);
        hash = HashU64(hash, offset);
        hash = HashU64(hash, fieldSize);
        hash = HashU64(hash, (uint64_t)shape.kind);
        hash = HashU64(hash, shape.arrayLength);
        hash = HashString(hash, shape.typeName);

        if (!described || (uint64_t)offset + fieldSize > size)
        {
            continue;
        }

        if (shape.kind == STRATA_TYPE_STRUCT && typeIndex != STRATA_TYPES_NO_INDEX)
        {
            const StructEntry* nested = (const StructEntry*)VecGet(&w->structs, typeIndex);
            hash = HashU64(hash, nested->layoutHash);

            for (uint32_t element = 0; element < elementCount; element++)
            {
                memcpy(defaults + offset + element * shape.elementSize, w->data.data + nested->defaultOffset,
                       shape.elementSize);
            }
        }
        else if (shape.kind == STRATA_TYPE_ENUM)
        {
            hash = HashU64(hash, (uint64_t)StorageKind(w, &shape));
        }

        if (field->folded.isSet && shape.arrayLength == 0)
        {
            WriteDefaultValue(defaults + offset, StorageKind(w, &shape), &field->folded);
        }
    }

    entry->firstAttribute = w->attributeCount;

    for (size_t i = 0; i < type->attributes.count; i++)
    {
        const Attribute* attribute = (const Attribute*)VecGet(&type->attributes, i);
        uint8_t record[STRATA_TYPES_ATTRIBUTE_RECORD_SIZE] = {0};
        PutU32(record, InternString(w, attribute->name));
        BufferAppend(&w->attributeRecords, record, sizeof(record));
        w->attributeCount++;
    }

    BufferAlign(&w->data, 16);
    entry->defaultOffset = (uint32_t)BufferAppend(&w->data, defaults, size);
    entry->layoutHash = hash;
    entry->nameString = InternString(w, type->name);
    entry->moduleNameString = InternString(w, type->moduleName);

    free(defaults);
}

static void WriteTable(uint8_t* header, uint32_t at, uint32_t count, uint32_t recordSize, uint32_t offset)
{
    PutU32(header + at, count);
    PutU32(header + at + 4, recordSize);
    PutU32(header + at + 8, offset);
}

static void WriterInit(Writer* w, const TypeRegistry* reg, const Module* mod)
{
    *w = (Writer){0};
    w->reg = reg;
    w->mod = mod;
    arena_init(&w->scratch, 1024);
    VecInit(&w->structs);
    VecInit(&w->visiting);
    VecInit(&w->enums);
    BufferAppend(&w->strings, "", 1);
}

static void WriterFree(Writer* w)
{
    BufferFree(&w->strings);
    BufferFree(&w->data);
    BufferFree(&w->fieldRecords);
    BufferFree(&w->attributeRecords);
    BufferFree(&w->enumValueRecords);
    free(w->structs.items);
    free(w->visiting.items);
    free(w->enums.items);
    arena_free(&w->scratch);
}

uint64_t TypeMetadataNameHash(const char* name)
{
    return HashBytes(FNV_OFFSET, name ? name : "", strlen(name ? name : ""));
}

uint64_t TypeMetadataLayoutHash(const TypeRegistry* reg, const Module* mod, const StructType* type)
{
    Writer w;
    WriterInit(&w, reg, mod);

    uint64_t hash = 0;

    if (VisitStruct(&w, type))
    {
        for (size_t i = 0; i < w.structs.count; i++)
        {
            StructEntry* entry = (StructEntry*)VecGet(&w.structs, i);
            WriteStruct(&w, entry);

            if (entry->type == type)
            {
                hash = entry->layoutHash;
            }
        }
    }

    WriterFree(&w);

    return hash;
}

bool TypeMetadataBuild(const TypeRegistry* reg, const Module* mod, uint8_t** outBytes, size_t* outSize)
{
    *outBytes = NULL;
    *outSize = 0;

    if (!reg || !mod)
    {
        return false;
    }

    Writer w;
    WriterInit(&w, reg, mod);

    for (size_t i = 0; i < reg->count; i++)
    {
        if (reg->types[i].isComponent)
        {
            VisitStruct(&w, &reg->types[i]);
        }
    }

    bool hasComponent = false;

    for (size_t i = 0; i < w.structs.count; i++)
    {
        StructEntry* entry = (StructEntry*)VecGet(&w.structs, i);
        WriteStruct(&w, entry);
        hasComponent |= entry->type->isComponent;
    }

    if (hasComponent)
    {
        for (size_t i = 0; i < w.enums.count; i++)
        {
            EnumEntry* entry = (EnumEntry*)VecGet(&w.enums, i);
            entry->nameString = InternString(&w, entry->decl->name);
            entry->firstValue = w.enumValueCount;

            for (size_t j = 0; j < entry->decl->members.count; j++)
            {
                const EnumMemberDecl* member = (const EnumMemberDecl*)VecGet(&entry->decl->members, j);
                uint8_t record[STRATA_TYPES_ENUM_VALUE_RECORD_SIZE] = {0};
                PutU32(record, InternString(&w, member->name));
                PutU64(record + 8, member->value);
                BufferAppend(&w.enumValueRecords, record, sizeof(record));
                w.enumValueCount++;
            }
        }

        uint32_t structOffset = STRATA_TYPES_HEADER_SIZE;
        uint32_t fieldOffset = structOffset + (uint32_t)w.structs.count * STRATA_TYPES_STRUCT_RECORD_SIZE;
        uint32_t attributeOffset = fieldOffset + w.fieldCount * STRATA_TYPES_FIELD_RECORD_SIZE;
        uint32_t enumOffset = attributeOffset + w.attributeCount * STRATA_TYPES_ATTRIBUTE_RECORD_SIZE;
        uint32_t enumValueOffset = enumOffset + (uint32_t)w.enums.count * STRATA_TYPES_ENUM_RECORD_SIZE;
        uint32_t stringOffset = enumValueOffset + w.enumValueCount * STRATA_TYPES_ENUM_VALUE_RECORD_SIZE;
        uint32_t dataOffset = (stringOffset + (uint32_t)w.strings.size + 15u) & ~15u;
        uint32_t totalSize = dataOffset + (uint32_t)w.data.size;

        uint8_t* blob = (uint8_t*)calloc(totalSize, 1);

        PutU32(blob, STRATA_TYPES_MAGIC);
        PutU16(blob + 4, STRATA_TYPES_VERSION);
        PutU16(blob + 6, STRATA_TYPES_HEADER_SIZE);
        PutU32(blob + 8, totalSize);
        blob[12] = 8; /* pointer size: handles are 64-bit on every supported target */
        WriteTable(blob, 16, (uint32_t)w.structs.count, STRATA_TYPES_STRUCT_RECORD_SIZE, structOffset);
        WriteTable(blob, 28, w.fieldCount, STRATA_TYPES_FIELD_RECORD_SIZE, fieldOffset);
        WriteTable(blob, 40, w.attributeCount, STRATA_TYPES_ATTRIBUTE_RECORD_SIZE, attributeOffset);
        WriteTable(blob, 52, (uint32_t)w.enums.count, STRATA_TYPES_ENUM_RECORD_SIZE, enumOffset);
        WriteTable(blob, 64, w.enumValueCount, STRATA_TYPES_ENUM_VALUE_RECORD_SIZE, enumValueOffset);
        PutU32(blob + 76, stringOffset);
        PutU32(blob + 80, (uint32_t)w.strings.size);
        PutU32(blob + 84, dataOffset);
        PutU32(blob + 88, (uint32_t)w.data.size);

        for (size_t i = 0; i < w.structs.count; i++)
        {
            const StructEntry* entry = (const StructEntry*)VecGet(&w.structs, i);
            uint8_t* record = blob + structOffset + i * STRATA_TYPES_STRUCT_RECORD_SIZE;

            PutU32(record, entry->nameString);
            PutU32(record + 4, entry->moduleNameString);
            PutU32(record + 8, (uint32_t)entry->type->sizeBytes);
            PutU32(record + 12, (uint32_t)entry->type->alignBytes);
            PutU32(record + 16, entry->type->isComponent ? STRATA_TYPES_STRUCT_COMPONENT : 0u);
            PutU32(record + 20, entry->firstField);
            PutU32(record + 24, (uint32_t)entry->type->fields.count);
            PutU32(record + 28, entry->firstAttribute);
            PutU32(record + 32, (uint32_t)entry->type->attributes.count);
            PutU32(record + 36, entry->defaultOffset);
            PutU64(record + 40, entry->layoutHash);
        }

        for (size_t i = 0; i < w.enums.count; i++)
        {
            const EnumEntry* entry = (const EnumEntry*)VecGet(&w.enums, i);
            uint8_t* record = blob + enumOffset + i * STRATA_TYPES_ENUM_RECORD_SIZE;

            PutU32(record, entry->nameString);
            record[4] = (uint8_t)entry->underlyingKind;
            PutU32(record + 8, entry->firstValue);
            PutU32(record + 12, (uint32_t)entry->decl->members.count);
        }

        memcpy(blob + fieldOffset, w.fieldRecords.data, w.fieldRecords.size);
        memcpy(blob + attributeOffset, w.attributeRecords.data, w.attributeRecords.size);
        memcpy(blob + enumValueOffset, w.enumValueRecords.data, w.enumValueRecords.size);
        memcpy(blob + stringOffset, w.strings.data, w.strings.size);
        memcpy(blob + dataOffset, w.data.data, w.data.size);

        *outBytes = blob;
        *outSize = totalSize;
    }

    WriterFree(&w);

    return *outBytes != NULL;
}

bool TypeMetadataBuildFromModule(const Module* mod, uint8_t** outBytes, size_t* outSize)
{
    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    bool built = TypeMetadataBuild(&reg, mod, outBytes, outSize);

    TypeRegistryFree(&reg);

    return built;
}
