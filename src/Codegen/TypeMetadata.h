#pragma once

#include "AST/AST.h"
#include "Codegen/TypeRegistry.h"

#include <stddef.h>
#include <stdint.h>

/* Serializes the module's `@component` structs (and the structs and enums they use) into the
   format described in strata/strata_types.h. `reg` must have layouts computed. Returns false,
   leaving *outBytes NULL, when the module has no components. The caller frees *outBytes. */
bool TypeMetadataBuild(const TypeRegistry* reg, const Module* mod, uint8_t** outBytes, size_t* outSize);

/* Sema-only path (no code generation): builds a registry for `mod` and serializes it. */
bool TypeMetadataBuildFromModule(const Module* mod, uint8_t** outBytes, size_t* outSize);

/* FNV-1a 64 of a type name, as StrataTypeDesc::nameHash. */
uint64_t TypeMetadataNameHash(const char* name);

/* The layout hash the metadata would record for `type` (0 when it has no layout). */
uint64_t TypeMetadataLayoutHash(const TypeRegistry* reg, const Module* mod, const StructType* type);
