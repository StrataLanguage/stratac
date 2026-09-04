#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"

/* A pad filling a `fieldoffset` gap; a trailing pad rounds out the struct. */
typedef struct {
    size_t beforeField;
    long bytes;
} StructPad;

typedef struct {
    const char* name;
    bool opaque;                 // `struct Foo;` or `handle Foo`.
    bool incomplete;             // Forward-declared struct.
    bool owning;                 // Holds an owning field, transitively.
    const char* extendsFrom;     // Base handle for `handle X extends Y`.
    Vec fields;
    bool isExtern;               // `extern struct`: mirrors a C layout.
    bool isTypeAlias;
    bool isEnum;                 /* `enum Foo` — a strong alias with scoped constants. */
    const char* underlyingType;  /* underlying type name for aliases */

    /* Computed layout; hasLayout is set only on success,
       layoutError holds the failure message. */
    bool hasLayout;
    bool packedLayout;  /* any explicit fieldoffset — backends emit packed */
    long sizeBytes;
    long alignBytes;
    long* fieldOffsets; /* logical field index -> byte offset */
    int* physicalIndex; /* logical field index -> physical member index */
    StructPad* pads;
    size_t padCount;
    size_t physicalCount; /* fields + pads */
    char* layoutError;
} StructType;

typedef struct TypeRegistry {
    StructType* types;
    size_t count;
    size_t cap;
} TypeRegistry;

void TypeRegistryInit(TypeRegistry* reg);
void TypeRegistryFree(TypeRegistry* reg);

// Registers aliases first so dims resolve before layout.
void TypeRegistryRegisterAliases(TypeRegistry* reg, const Module* m);
void TypeRegistryBuild(TypeRegistry* reg, const Module* m);
void ComputeAllLayouts(TypeRegistry* reg);
const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsUserType(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsOpaque(const TypeRegistry* reg, const char* name);
// Any registered type works as an `impl` target.
bool TypeRegistryIsImplTarget(const TypeRegistry* reg, const char* name);
int TypeRegistryFieldIndex(const TypeRegistry* reg, const char* structName, const char* field);

// Shape queries are structural (see TypeNameIs* in AST.h).
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

/* Type alias queries. */
bool TypeRegistryIsTypeAlias(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsEnum(const TypeRegistry* reg, const char* name);
const char* TypeRegistryGetUnderlyingType(const TypeRegistry* reg, const char* name);
// Resolve aliases to the final underlying type.
const char* TypeRegistryResolveAlias(const TypeRegistry* reg, const char* name);

// The moved lvalue (identifier, member chain, or element), casts unwrapped.
const Node* MovableBoxSourceNode(const Node* n);

// True for assignable lvalues.
bool IsLValueNode(const Node* n);

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);
