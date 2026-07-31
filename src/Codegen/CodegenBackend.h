#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

typedef struct {
    bool ok;
    char* output;
    char* moduleName;
} CodegenResult;

CodegenResult GenerateLlvmIr(const Module* mod);
char* DumpAst(const Module* mod, Arena* arena);
