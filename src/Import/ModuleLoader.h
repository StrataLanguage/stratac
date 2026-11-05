#pragma once

// Module loader: resolves imports from disk or a host resolver, and merges them.

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Core/Util.h"

#include "strata/strata.h"

#include <stddef.h>
#include <stdint.h>

// Returns in-memory text (borrowed for the load) that replaces the file at `canonPath`, or NULL to read it from
// disk. Editors use this to analyze unsaved buffers.
typedef const char* (*ModuleOverlayFn)(void* userData, const char* canonPath, size_t* lenOut);

typedef struct
{
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

    // Directories searched, in order, for an import not found next to its importer.
    const char* const* importDirs;
    size_t importDirCount;

    // Consulted before disk for imports found on the file system (NULL = disk only).
    ModuleOverlayFn overlay;
    void* overlayUserData;
} ModuleLoader;

void ModuleLoaderInit(ModuleLoader* l, Arena* arena, DiagnosticEngine* diag);
void ModuleLoaderDispose(ModuleLoader* l);

// Install (or clear with fn=NULL) a custom import resolver.
void ModuleLoaderSetResolver(ModuleLoader* l, StrataImportResolverFn fn, void* userData);

// Directories (borrowed) searched for imports not found relative to the importing file.
void ModuleLoaderSetImportDirs(ModuleLoader* l, const char* const* dirs, size_t count);

// Load from disk; imports use the resolver when set.
Module* ModuleLoaderLoad(ModuleLoader* l, const char* mainPath);

// Load from memory; `text` is borrowed. Needs a resolver for imports.
Module* ModuleLoaderLoadSource(ModuleLoader* l, const char* name, const char* text, size_t textLen);

// Load the file at `path` from memory (`text` is borrowed), as if it were on disk: imports resolve next to
// it, and an import cycle that reaches `path` reuses this text.
Module* ModuleLoaderLoadBuffer(ModuleLoader* l, const char* path, const char* text, size_t textLen);

// Overlay buffers (see ModuleOverlayFn) for files found on disk or through the import directories.
void ModuleLoaderSetOverlay(ModuleLoader* l, ModuleOverlayFn fn, void* userData);

// The canonical spelling of `path` that the loader uses for files and passes to the overlay.
const char* ModuleLoaderCanonicalPath(Arena* arena, const char* path);
