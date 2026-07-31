#pragma once

#include "strata/AST/AST.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Core/Util.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    Arena* arena;
    DiagnosticEngine* diag;

    SourceManager* sources;
    size_t sourceCount;
    size_t sourceCap;

    char** buffers;
    size_t bufferCount;
    size_t bufferCap;

    const char** visited;
    size_t visitedCount;
    size_t visitedCap;

    Module* root;
} ModuleLoader;

void ModuleLoaderInit(ModuleLoader* l, Arena* arena, DiagnosticEngine* diag);
void ModuleLoaderDispose(ModuleLoader* l);

Module* ModuleLoaderLoad(ModuleLoader* l, const char* mainPath);
