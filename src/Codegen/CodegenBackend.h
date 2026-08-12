#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

#include <strata/strata.h>

typedef struct {
    bool ok;
    char* output;
    char* moduleName;
} CodegenResult;


CodegenResult GenerateLlvmIr(const Module* mod);
CodegenResult GenerateC(const Module* mod, StrataArch arch);
char* DumpAst(const Module* mod, Arena* arena);
