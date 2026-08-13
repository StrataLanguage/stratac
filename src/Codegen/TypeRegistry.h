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

/* Owning (heap-allocated, move-only) type helpers.
 * Covers box<T>, string, and T[] (arrays own their backing buffer). */
bool IsOwningType(const char* name);
Str  OwningInnerStr(const char* name);             /* Str slice - no alloc (box inner only) */
const char* OwningInnerCStr(Arena* arena, const char* name); /* null-terminated arena copy */
bool TypeRegistryIsOwningStruct(const TypeRegistry* reg, const char* name);

/* Returns the lvalue actually being moved (identifier, member chain, or array
   element), unwrapping casts; NULL if `n` is not a movable box source. Shared
   by the LLVM and C backends (source nulling) and sema (move tracking), so the
   "what can be moved" rule lives in one place. */
const Node* MovableBoxSourceNode(const Node* n);

/* Pure "is this an assignable lvalue expression" check (identifier, member
   chain, or array element) WITHOUT unwrapping casts - used where a call site
   decides between taking an lvalue's address and materializing a temp. */
bool IsLValueNode(const Node* n);

/* Array type helpers. An array type name is encoded as "<inner>[]"
 * (e.g. "int[]"). Internally a fat {ptr, u64} struct: heap data + length. */
bool IsArrayType(const char* name);
Str  ArrayInnerStr(const char* name);              /* Str slice of the element type — no alloc */

// -- Helpers

bool HandleExtendsFrom(const TypeRegistry* reg, const char* derived, const char* base);
bool IsHandleType(const TypeRegistry* reg, const char* name);