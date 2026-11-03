#pragma once
// TypeDesc shape-flag predicates.

#include "Codegen/LLVMModuleBuilder.h"

// The shape flags of a TypeDesc are read through these predicates, so code outside the type layer never
// tests `isBox`, `isOptional` or `isArray` itself.

static inline void TypeDescSetFlag(TypeDesc* t, TypeDescFlags flag, bool v)
{
    if (v)
    {
        t->flags |= flag;
    }
    else
    {
        t->flags &= ~flag;
    }
}

static inline bool TypeDescIsFloat(const TypeDesc* t)
{
    return (t->flags & TDF_FLOAT) != 0;
}

static inline bool TypeDescIsUnsigned(const TypeDesc* t)
{
    return (t->flags & TDF_UNSIGNED) != 0;
}

static inline bool TypeDescIsVoid(const TypeDesc* t)
{
    return (t->flags & TDF_VOID) != 0;
}

static inline bool TypeDescIsBox(const TypeDesc* t)
{
    return (t->flags & TDF_BOX) != 0;
}

static inline bool TypeDescIsOptional(const TypeDesc* t)
{
    return (t->flags & TDF_OPTIONAL) != 0;
}

static inline bool TypeDescHasArrayShape(const TypeDesc* t)
{
    return (t->flags & TDF_ARRAY) != 0;
}

static inline bool TypeDescIsString(const TypeDesc* t)
{
    return (t->flags & TDF_STRING) != 0;
}

static inline bool TypeDescIsCString(const TypeDesc* t)
{
    return (t->flags & TDF_CSTRING) != 0;
}

static inline bool TypeDescIsSimdVector(const TypeDesc* t)
{
    return (t->flags & TDF_SIMD_VECTOR) != 0;
}

static inline bool TypeDescIsAliasedArray(const TypeDesc* t)
{
    return (t->flags & TDF_ALIASED_ARRAY) != 0;
}

static inline bool TypeDescIsFixedArray(const TypeDesc* t)
{
    return (t->flags & TDF_FIXED_ARRAY) != 0;
}

static inline void TypeDescSetFloat(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_FLOAT, v);
}

static inline void TypeDescSetUnsigned(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_UNSIGNED, v);
}

static inline void TypeDescSetVoid(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_VOID, v);
}

static inline void TypeDescSetBox(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_BOX, v);
}

static inline void TypeDescSetOptional(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_OPTIONAL, v);
}

static inline void TypeDescSetArrayShape(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_ARRAY, v);
}

static inline void TypeDescSetString(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_STRING, v);
}

static inline void TypeDescSetCString(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_CSTRING, v);
}

static inline void TypeDescSetSimdVector(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_SIMD_VECTOR, v);
}

static inline void TypeDescSetAliasedArray(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_ALIASED_ARRAY, v);
}

static inline void TypeDescSetFixedArray(TypeDesc* t, bool v)
{
    TypeDescSetFlag(t, TDF_FIXED_ARRAY, v);
}
