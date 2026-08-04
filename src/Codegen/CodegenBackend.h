#pragma once

#include "AST/AST.h"
#include "Core/Util.h"

typedef struct {
    bool ok;
    char* output;
    char* moduleName;
} CodegenResult;

enum StrataArch : int;

CodegenResult GenerateLlvmIr(const Module* mod);
CodegenResult GenerateC(const Module* mod, enum StrataArch arch);
char* DumpAst(const Module* mod, Arena* arena);
