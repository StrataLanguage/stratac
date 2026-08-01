#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"

typedef struct {
    const char* name;
    bool opaque;
    bool incomplete;
    bool owning;        /* transitively contains a box<T> field */
    const char* extendsFrom;
    Vec fields;
} StructType;

typedef struct {
    StructType* types;
    size_t count;
    size_t cap;
} TypeRegistry;

void TypeRegistryInit(TypeRegistry* reg);
void TypeRegistryFree(TypeRegistry* reg);
void TypeRegistryBuild(TypeRegistry* reg, const Module* m);
const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsUserType(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsOpaque(const TypeRegistry* reg, const char* name);
int TypeRegistryFieldIndex(const TypeRegistry* reg, const char* structName, const char* field);

/* box<T> type-name helpers (the name is encoded as "box<INNER>"). */
bool IsBoxTypeName(const char* name);
bool BoxInnerTypeName(const char* name, char* buf, size_t cap);
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);