#pragma once

#include "Codegen/LLVMModuleBuilder.h"

/* Roadmap phase 3: the shape flags of a TypeDesc are read through these predicates, so code outside the
   type layer never tests `isBox`, `isOptional` or `isArray` itself. */

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
