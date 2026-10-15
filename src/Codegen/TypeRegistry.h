#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include "Codegen/TypeUtil.h"
#include "PrimitiveTypes.h"

/* Builtin handle types registered in every module (see TypeRegistryBuild). */
#define STRATA_PTR_TYPE "ptr"
#define STRATA_NULL_TYPE "null"

/* The hidden field of a struct with a `Drop` method: 1 while it holds a live value, 0 once moved or dropped. */
#define STRATA_ALIVE_FIELD "__alive"

/* `null`, the builtin constant (of type `null`) that converts to any handle. */
static inline bool IsNullConstantName(const char* name)
{
    return name && strcmp(name, "null") == 0;
}

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
    const char* extendsFrom;     // Base handle for `handle X : Y`.
    Vec fields;
    bool isExtern;               // `extern struct`: mirrors a C layout.
    bool isUnion;                // `union`: every field at offset 0.
    const struct FunctionDecl* delegate; /* `extern delegate`: the handle's call signature */
    const struct FunctionDecl* dropFunction; /* the struct's `Drop` method, NULL if none */
    int unionMember;             /* union: the field the backend lays out (the most aligned, then largest);
                                    every field's physical index is 0, a tail pad rounds out the size */
    bool isTypeAlias;
    bool isEnum;                 /* `enum Foo` — a strong alias with scoped constants. */
    const char* underlyingType;  /* underlying type name for aliases */
    PrimitiveType underlyingPrimitiveType; /* underlying type value for the alias */
    bool isComponent;            /* `@component` */
    Vec attributes;              /* Vec<Attribute*>, shared with the StructDecl */
    const char* moduleName;      /* module the struct was declared in */

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

// Whether a value of type `t` holds anything with a `Drop` method (by value, in an array or a box): such
// values move, and are never implicitly deep-copied.
bool TypeNameHasDrop(const TypeRegistry* reg, const TypeName* t);

// The signature of an `extern delegate` type, or NULL when `name` is not one.
const struct FunctionDecl* TypeRegistryDelegate(const TypeRegistry* reg, const char* name);
