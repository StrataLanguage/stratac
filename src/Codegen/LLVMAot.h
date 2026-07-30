#pragma once

#include "LLVMModuleBuilder.h"

bool EmitNativeFile(BuiltModule* bm, const char* path, bool assembly, char** errorMessage, const char* targetTriple);
