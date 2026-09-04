#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Core/Util.h"

#include "strata/strata.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    Arena* arena;
    DiagnosticEngine* diag;

    // Host import resolver (NULL = load from disk).
    StrataImportResolverFn resolver;
    void* resolverUserData;

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

// Install (or clear with fn=NULL) a custom import resolver.
void ModuleLoaderSetResolver(ModuleLoader* l, StrataImportResolverFn fn, void* userData);

// Load from disk; imports use the resolver when set.
Module* ModuleLoaderLoad(ModuleLoader* l, const char* mainPath);

// Load from memory; `text` is borrowed. Needs a resolver for imports.
Module* ModuleLoaderLoadSource(ModuleLoader* l, const char* name, const char* text, size_t textLen);
