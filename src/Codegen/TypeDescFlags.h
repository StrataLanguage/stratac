#pragma once

#include "Codegen/LLVMModuleBuilder.h"

/* Roadmap phase 3: the shape flags of a TypeDesc are read through these predicates, so code outside the
   type layer never tests `isBox`, `isOptional` or `isArray` itself. */

static inline bool TypeDescIsFloat(const TypeDesc* t)
{
    return t->isFloat;
}

static inline bool TypeDescIsUnsigned(const TypeDesc* t)
{
    return t->isUnsigned;
}

static inline bool TypeDescIsVoid(const TypeDesc* t)
{
    return t->isVoid;
}

static inline bool TypeDescIsBox(const TypeDesc* t)
{
    return t->isBox;
}

static inline bool TypeDescIsOptional(const TypeDesc* t)
{
    return t->isOptional;
}

static inline bool TypeDescHasArrayShape(const TypeDesc* t)
{
    return t->isArray;
}

static inline bool TypeDescIsString(const TypeDesc* t)
{
    return t->isString;
}

static inline bool TypeDescIsCString(const TypeDesc* t)
{
    return t->isCString;
}

static inline bool TypeDescIsSimdVector(const TypeDesc* t)
{
    return t->isSimdVector;
}

static inline bool TypeDescIsAliasedArray(const TypeDesc* t)
{
    return t->aliasedArray;
}

static inline bool TypeDescIsFixedArray(const TypeDesc* t)
{
    return t->isFixedArray;
}

static inline void TypeDescSetFloat(TypeDesc* t, bool v)
{
    t->isFloat = v;
}

static inline void TypeDescSetUnsigned(TypeDesc* t, bool v)
{
    t->isUnsigned = v;
}

static inline void TypeDescSetVoid(TypeDesc* t, bool v)
{
    t->isVoid = v;
}

static inline void TypeDescSetBox(TypeDesc* t, bool v)
{
    t->isBox = v;
}

static inline void TypeDescSetOptional(TypeDesc* t, bool v)
{
    t->isOptional = v;
}

static inline void TypeDescSetArrayShape(TypeDesc* t, bool v)
{
    t->isArray = v;
}

static inline void TypeDescSetString(TypeDesc* t, bool v)
{
    t->isString = v;
}

static inline void TypeDescSetCString(TypeDesc* t, bool v)
{
    t->isCString = v;
}

static inline void TypeDescSetSimdVector(TypeDesc* t, bool v)
{
    t->isSimdVector = v;
}

static inline void TypeDescSetAliasedArray(TypeDesc* t, bool v)
{
    t->aliasedArray = v;
}

static inline void TypeDescSetFixedArray(TypeDesc* t, bool v)
{
    t->isFixedArray = v;
}
