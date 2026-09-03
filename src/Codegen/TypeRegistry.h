#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"

/* A pad inserted into the physical member list where explicit `fieldoffset`
   markers (or the gaps they create) require it. A trailing pad (struct
   rounding) uses beforeField == fields.count. */
typedef struct {
    size_t beforeField;
    long bytes;
} StructPad;

typedef struct {
    const char* name;
    bool opaque;
    bool incomplete;
    bool owning;        /* transitively contains a ^T field */
    const char* extendsFrom;
    Vec fields;
    bool isExtern;      /* `extern struct` — mirrors a host-defined layout */
    bool isTypeAlias;   /* `struct Foo = uint;` — strongly typed alias */
    const char* underlyingType; /* underlying type name for aliases */
    /* Computed layout (complete, non-opaque structs). hasLayout is only set
       when the layout is valid; on failure layoutError holds a malloc'd
       message that sema reports. */
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
/* Registers only the module's type aliases (idempotent). Sema calls this
   before full registration so alias resolution works while manifest-constant
   array dimensions resolve, ahead of layout computation. */
void TypeRegistryRegisterAliases(TypeRegistry* reg, const Module* m);
void TypeRegistryBuild(TypeRegistry* reg, const Module* m);
void ComputeAllLayouts(TypeRegistry* reg);
const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsUserType(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsOpaque(const TypeRegistry* reg, const char* name);
/* An `impl` target: any registered non-alias type — handles, defined
   structs, and forward-declared (incomplete) structs alike. */
bool TypeRegistryIsImplTarget(const TypeRegistry* reg, const char* name);
int TypeRegistryFieldIndex(const TypeRegistry* reg, const char* structName, const char* field);

/* Type shape queries (arrays, boxes, owning-ness) are structural — see the
   TypeNameIs* accessors in AST/AST.h. The canonical `name` spelling is
   display/mangling data only. */
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

/* Type alias queries. */
bool TypeRegistryIsTypeAlias(const TypeRegistry* reg, const char* name);
const char* TypeRegistryGetUnderlyingType(const TypeRegistry* reg, const char* name);
/* Recursively resolves type aliases to their final non-alias underlying type. */
const char* TypeRegistryResolveAlias(const TypeRegistry* reg, const char* name);

/* Returns the lvalue actually being moved (identifier, member chain, or array
   element), unwrapping casts */
const Node* MovableBoxSourceNode(const Node* n);

/* is this an assignable lvalue expression? */
bool IsLValueNode(const Node* n);

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);
