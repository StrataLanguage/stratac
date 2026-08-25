#pragma once

#include "LLVMModuleBuilder.h"

bool EmitNativeFile(BuiltModule* bm, const char* path, bool assembly, char** errorMessage, const char* targetTriple);

/* Compiles the module to a native object file in memory (host target).
   Returns malloc-owned bytes (freed by the caller) and the size, or NULL
   with *errorMessage (malloc-owned) set. Consumes nothing: the caller keeps
   ownership of bm. */
char* EmitNativeMemory(BuiltModule* bm, size_t* outSize, char** errorMessage);
