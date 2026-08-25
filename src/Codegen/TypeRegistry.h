#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"

typedef struct {
    const char* name;
    bool opaque;
    bool incomplete;
    bool owning;        /* transitively contains a ^T field */
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

//-- Box helpers
bool IsOwningType(const char* name);
Str  OwningInnerStr(const char* name);             /* Str slice - no alloc (box inner only) */
const char* OwningInnerCStr(Arena* arena, const char* name); /* null-terminated arena copy */
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

/* Returns the lvalue actually being moved (identifier, member chain, or array
   element), unwrapping casts */
const Node* MovableBoxSourceNode(const Node* n);

/* is this an assignable lvalue expression? */
bool IsLValueNode(const Node* n);

//-- Arrays
bool IsArrayType(const char* name);
Str ArrayInnerStr(const char* name);

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);