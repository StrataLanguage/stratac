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

    /* Optional host-provided import resolver. When set, every `import X;` is
       resolved through it instead of the filesystem. NULL = filesystem mode. */
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

/* Install (or clear with fn=NULL) a custom import resolver. */
void ModuleLoaderSetResolver(ModuleLoader* l, StrataImportResolverFn fn, void* userData);

/* Load a module graph starting from a filesystem path. Imports are resolved
   through the resolver if one is set, otherwise relative to the filesystem. */
Module* ModuleLoaderLoad(ModuleLoader* l, const char* mainPath);

/* Load a module graph starting from an in-memory source string. `text` is
   borrowed (must outlive the loader); `name` is the module/display name.
   Imports are resolved through the resolver (a resolver MUST be set, since
   there is no filesystem context for relative resolution). */
Module* ModuleLoaderLoadSource(ModuleLoader* l, const char* name, const char* text, size_t textLen);
