#pragma once
// AOT object emission from a built LLVM module.

#include "LLVMModuleBuilder.h"

bool EmitNativeFile(BuiltModule* bm, const char* path, bool assembly, char** errorMessage, const char* targetTriple);
