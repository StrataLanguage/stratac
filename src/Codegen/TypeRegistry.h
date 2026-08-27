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

typedef struct {
    StructType* types;
    size_t count;
    size_t cap;
} TypeRegistry;

void TypeRegistryInit(TypeRegistry* reg);
void TypeRegistryFree(TypeRegistry* reg);
void TypeRegistryBuild(TypeRegistry* reg, const Module* m);
void ComputeAllLayouts(TypeRegistry* reg);
const StructType* TypeRegistryFind(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsUserType(const TypeRegistry* reg, const char* name);
bool TypeRegistryIsOpaque(const TypeRegistry* reg, const char* name);
int TypeRegistryFieldIndex(const TypeRegistry* reg, const char* structName, const char* field);

/* Type shape queries (arrays, boxes, owning-ness) are structural — see the
   TypeNameIs* accessors in AST/AST.h. The canonical `name` spelling is
   display/mangling data only. */
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

/* Returns the lvalue actually being moved (identifier, member chain, or array
   element), unwrapping casts */
const Node* MovableBoxSourceNode(const Node* n);

/* is this an assignable lvalue expression? */
bool IsLValueNode(const Node* n);

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);
