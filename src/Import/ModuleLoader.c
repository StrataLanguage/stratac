#include "Import/ModuleLoader.h"

#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Core/Util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t GrowCap(size_t cap)
{
    return cap ? cap * 2 : 8;
}

static char* ResolveImportPath(Arena* arena, const char* importerPath, const char* importPath)
{
    size_t impLen = strlen(importPath);

    bool hasExt = impLen >= 7 && strcmp(importPath + impLen - 7, ".strata") == 0;
    const char* ext = hasExt ? "" : ".strata";

    size_t dirLen = DirLen(importerPath);
    if (dirLen > 0)
    {
        return arena_format(arena, "%.*s/%s%s", (int)dirLen, importerPath, importPath, ext);
    }

    return arena_format(arena, "%s%s", importPath, ext);
}

static void PushBuffer(ModuleLoader* loader, char* buf)
{
    if (loader->bufferCount >= loader->bufferCap)
    {
        loader->bufferCap = GrowCap(loader->bufferCap);
        loader->buffers = (char**)realloc(loader->buffers, loader->bufferCap * sizeof(char*));
    }
    loader->buffers[loader->bufferCount++] = buf;
}

static void PushVisited(ModuleLoader* loader, const char* path)
{
    if (loader->visitedCount >= loader->visitedCap)
    {
        loader->visitedCap = GrowCap(loader->visitedCap);
        loader->visited = (const char**)realloc(loader->visited, loader->visitedCap * sizeof(const char*));
    }

    loader->visited[loader->visitedCount++] = path;
}

static bool AlreadyVisited(const ModuleLoader* loader, const char* path)
{
    for (size_t i = 0; i < loader->visitedCount; i++)
    {
        if (strcmp(loader->visited[i], path) == 0)
        {
            return true;
        }
    }

    return false;
}

static void AppendItems(Module* root, const Module* src)
{
    for (size_t i = 0; i < src->structs.count; i++)
    {
        VecPush(&root->structs, VecGet(&src->structs, i));
    }

    for (size_t i = 0; i < src->handles.count; i++)
    {
        VecPush(&root->handles, VecGet(&src->handles, i));
    }

    for (size_t i = 0; i < src->functions.count; i++)
    {
        VecPush(&root->functions, VecGet(&src->functions, i));
    }
    
    for (size_t i = 0; i < src->globals.count; i++)
    {
        VecPush(&root->globals, VecGet(&src->globals, i));
    }
}

/* Forward declarations. */
static void LoadModule(ModuleLoader* loader, const char* name, const char* text, size_t textLen, bool textOwned);
static void ResolveImport(ModuleLoader* loader, const char* importerName, const char* importPath);

static void LoadModule(ModuleLoader* loader, const char* name, const char* text, size_t textLen, bool textOwned)
{
    /* Take ownership of caller-owned text before any early return so it can
       never leak. */
    if (textOwned)
    {
        PushBuffer(loader, (char*)text);
    }

    if (AlreadyVisited(loader, name))
    {
        return;
    }

    const char* nameKey = arena_strdup(loader->arena, name);
    PushVisited(loader, nameKey);

    if (loader->sourceCount >= loader->sourceCap)
    {
        loader->sourceCap = GrowCap(loader->sourceCap);
        loader->sources = (SourceManager*)realloc(loader->sources, loader->sourceCap * sizeof(SourceManager));
    }

    uint16_t fileId = (uint16_t)loader->sourceCount;

    SourceManager* sm = &loader->sources[fileId];
    SourceManagerInit(sm);
    SourceManagerSetSource(sm, text, textLen, nameKey);

    loader->sourceCount++;

    Lexer lex;
    LexerInit(&lex, sm->m_text, sm->m_textLen, loader->diag, fileId);

    Parser parser;
    ParserInit(&parser, &lex, loader->diag, loader->arena, nameKey);

    Module* fileMod = ParserParseModule(&parser);
    if (!fileMod)
    {
        return;
    }

    for (size_t i = 0; i < fileMod->imports.count; i++)
    {
        ImportDecl* imp = (ImportDecl*)VecGet(&fileMod->imports, i);
        ResolveImport(loader, nameKey, imp->importPath);
    }

    AppendItems(loader->root, fileMod);
    AstReleaseModuleLists(fileMod);
}

static void ResolveImport(ModuleLoader* loader, const char* importerName, const char* importPath)
{
    if (loader->resolver)
    {
        StrataResolvedModule resolved = {0};
        int ok = loader->resolver(loader->resolverUserData, importerName, importPath, &resolved);

        if (!ok || !resolved.text)
        {
            DiagErrorFmt(loader->diag, SRC_INVALID, "cannot resolve import '%s'", importPath);
            return;
        }

        const char* childName = resolved.name ? resolved.name : importPath;

        /* `name` is copied inside LoadModule; `text` is borrowed for the
           duration of the compile (the host owns it). */
        LoadModule(loader, childName, resolved.text, resolved.length, false);
        return;
    }

    char* childPath = ResolveImportPath(loader->arena, importerName, importPath);

    /* Skip re-reading disk files we've already loaded. */
    if (AlreadyVisited(loader, childPath))
    {
        return;
    }

    size_t fileLen = 0;
    char* source = ReadWholeFile(childPath, &fileLen);
    if (!source)
    {
        DiagErrorFmt(loader->diag, SRC_INVALID, "cannot open module '%s'", childPath);
        return;
    }

    LoadModule(loader, childPath, source, fileLen, true);
}

static Module* NewRootModule(Arena* arena, const char* name)
{
    Module* root = AST_NEW(arena, Module);
    root->base.kind = NodeModule;
    root->base.range = SRC_INVALID;
    root->name = arena_strdup(arena, name);
    VecInit(&root->structs);
    VecInit(&root->handles);
    VecInit(&root->functions);
    VecInit(&root->globals);
    VecInit(&root->imports);

    return root;
}

void ModuleLoaderInit(ModuleLoader* loader, Arena* arena, DiagnosticEngine* diag)
{
    *loader = (ModuleLoader){0};
    loader->arena = arena;
    loader->diag = diag;
}

void ModuleLoaderSetResolver(ModuleLoader* loader, StrataImportResolverFn fn, void* userData)
{
    loader->resolver = fn;
    loader->resolverUserData = userData;
}

void ModuleLoaderDispose(ModuleLoader* loader)
{
    for (size_t i = 0; i < loader->sourceCount; i++)
    {
        SourceManagerFree(&loader->sources[i]);
    }

    free(loader->sources);

    for (size_t i = 0; i < loader->bufferCount; i++)
    {
        free(loader->buffers[i]);
    }

    free(loader->buffers);
    free(loader->visited);

    *loader = (ModuleLoader){0};
}

Module* ModuleLoaderLoad(ModuleLoader* loader, const char* mainPath)
{
    loader->root = NewRootModule(loader->arena, mainPath);

    /* The main file is always supplied by the caller from disk; only its
       imports are routed through the resolver (if one is set). */
    size_t fileLen = 0;
    char* source = ReadWholeFile(mainPath, &fileLen);
    if (!source)
    {
        DiagErrorFmt(loader->diag, SRC_INVALID, "cannot open module '%s'", mainPath);
        return loader->root;
    }

    LoadModule(loader, mainPath, source, fileLen, true);

    return loader->root;
}

Module* ModuleLoaderLoadSource(ModuleLoader* loader, const char* name, const char* text, size_t textLen)
{
    loader->root = NewRootModule(loader->arena, name ? name : "<string>");

    /* The main source is the caller's string (borrowed). Imports require a
       resolver since there is no filesystem context for relative resolution. */
    LoadModule(loader, loader->root->name, text, textLen, false);

    return loader->root;
}
