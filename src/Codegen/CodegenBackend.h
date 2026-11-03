#pragma once
// Interface shared by the C and LLVM code generators.

#include "AST/AST.h"
#include "Core/Util.h"

#include <strata/strata.h>

typedef struct
{
    bool ok;
    char* output;
    char* moduleName;
} CodegenResult;

CodegenResult GenerateLlvmIr(const Module* mod);
char* DumpAst(const Module* mod, Arena* arena);
