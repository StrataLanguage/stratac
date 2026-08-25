#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"

/* A padding member inserted into the physical member list of a struct whose
   layout contains explicit `fieldoffset` markers (or gaps created by them).
   `beforeField` is the logical field index the pad precedes; a trailing pad
   (rounding the struct to its final size) uses beforeField == fields.count. */
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
/* Dynamic `T[]` (fat {ptr, len} pointer). NOTE: returns false for fixed-size
   `T[N]` — use IsFixedArrayType for those. */
bool IsArrayType(const char* name);
Str ArrayInnerStr(const char* name); /* strips the first bracket group; empty when the
                                        result is not contiguous (nested fixed arrays) */
char* ArrayInnerName(Arena* arena, const char* name); /* allocating variant, handles nesting */

/* Fixed-size `T[N]` — C-ABI inline storage (struct fields only). Dimensions
   are spelled in source order, outermost first (`int[2][6]` = 2 x int[6]). */
bool IsFixedArrayType(const char* name);
long FixedArrayLength(const char* name); /* -1 when not a fixed array */

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);
