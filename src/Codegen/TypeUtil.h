#pragma once

#include "AST/AST.h"

#include <stdbool.h>

typedef struct {
    bool valid;
    bool isVoid;
    bool isFloat;
    bool isUnsigned;
    bool isSimdVector;
    int bits;

    // SIMD lane count (2/4), 0 when not a vector.
    int lanes;

    char elemIr[16];
    char ir[32];
} MappedType;

/* Full definition lives in TypeRegistry.h. */
typedef struct TypeRegistry TypeRegistry;
typedef struct Arena Arena;

MappedType MapType(const TypeName* t);

bool IsNumeric(PrimitiveType primType);
bool IsNameNumeric(const char* t);


int IsSimdVector(PrimitiveType prim);



/**
 * @brief Returns 0 if not a SIMD vector. Otherwise, returns the number of lanes for the vector.
 * Note that this does not represent the exact amount of lanes of the underlying vector, just the ones exposed to the user.
 * Even though a `float3` is still an `__m128` or `float32x4_t`, it will return 3 lanes.
 */
int IsNameSimdVector(const char* t);

#define GetSimdVectorLanes(t_) IsSimdVector(t_)
#define GetNameSimdVectorLanes(t_) IsNameSimdVector(t_)

bool IsScalarType(PrimitiveType prim);
bool IsNameScalarType(const char* t);

bool IsStringType(PrimitiveType prim);

// `cstring` — constant NUL-terminated string (single `const char*`, `.rodata`).
bool IsCStringType(PrimitiveType prim);

bool IsFloatType(PrimitiveType prim);
bool IsNameFloatType(const char* t);

bool IsScalarPseudoType(PrimitiveType prim);

/* Builtin numeric scalars eligible for `max`/`min` pseudo-properties:
   int/uint/long/ulong/byte/sbyte/short/ushort/float/double (NOT bool).
   True only for the BUILTIN names — never aliases or user types. */
bool IsNameScalarPseudoType(const char* t);

/* Scalar pseudo-property read (`int.max`, `float.min`): `t` must be a
   builtin scalar pseudo type and `member` must be "max" (every scalar) or
   "min" (float/double only). When valid, fills the constant value: `outInt`
   gets the integer bit pattern (ints), `outFloat` the double (floats),
   `outIsFloat` distinguishes them. Any output may be NULL. */
bool ScalarPseudoConst(PrimitiveType primitive, const char* member, uint64_t* outInt, double* outFloat, bool* outIsFloat);



// Alias-aware scalar check; NULL registry means builtins only.
bool IsScalarLikeType(const TypeRegistry* reg, const char* t);

// True when the (alias-resolved) name is "string".
bool TypeIsString(const TypeRegistry* reg, const char* name);

// True when the (alias-resolved) name is "cstring".
bool TypeIsCString(const TypeRegistry* reg, const char* name);

/* True when `==`/`!=` (and ordering) compares the value directly — scalars,
   handles, enums, and aliases of those compare by value / pointer identity.
   NOT true for strings (CONTENT equality via codegen `strata_str_eq`) or for
   aggregates — arrays and structs take the structural-equality path instead
   (see TypeIsComparableAggregate); this is the sema fallback for everything
   that still compares as a single value. */
bool TypeIsTriviallyComparable(const TypeRegistry* reg, const TypeName* t);

/* True for equality-comparable AGGREGATE types: dynamic and fixed arrays
   (`T[]`, `T[N]`, and optional `T[]?`), and DEFINED structs (plain or
   extern — never opaque/handle/alias/enum, which stay value/pointer
   compares). `==`/`!=` on these is structural (codegen emits per-type
   `strata_eq_*` helpers: memcmp where the layout allows, member-wise /
   element-wise recursion otherwise); ordering comparisons are rejected. */
bool TypeIsComparableAggregate(const TypeRegistry* reg, const TypeName* t);

// True when the type owns (box/array/string, or alias of one).
bool TypeIsOwningResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t);

/* True when a VALUE of the type owns heap data: everything
   TypeIsOwningResolved covers, plus owning structs (defined structs with a
   transitive owning field — `Rec` when `struct Rec { string s; };`), their
   aliases included. This is the predicate that decides move/drop/copy
   treatment wherever an owning-struct value can appear (notably as a `T[]`
   element, where `Rec[]` is legal inline storage). */
bool TypeIsOwningValueResolved(const TypeRegistry* reg, Arena* arena, const TypeName* t);
