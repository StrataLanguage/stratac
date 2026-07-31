#include "Import/ModuleLoader.h"

#include "strata/Lex/Lexer.h"
#include "strata/Parse/Parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* ReadFileAlloc(const char* path, size_t* outLen)
{
    FILE* in = fopen(path, "rb");
    if (!in)
    {
        return NULL;
    }

    if (fseek(in, 0, SEEK_END) != 0)
    {
        fclose(in);
        return NULL;
    }

    long size = ftell(in);
    if (size < 0)
    {
        fclose(in);
        return NULL;
    }

    rewind(in);

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(in);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, in);
    fclose(in);

    buf[n] = '\0';

    if (outLen)
    {
        *outLen = n;
    }

    return buf;
}

static size_t DirLen(const char* path)
{
    size_t len = strlen(path);
    size_t i = len;

    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\')
    {
        --i;
    }

    return i > 0 ? i - 1 : 0;
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
        loader->bufferCap = loader->bufferCap ? loader->bufferCap * 2 : 8;
        loader->buffers = (char**)realloc(loader->buffers, loader->bufferCap * sizeof(char*));
    }
    loader->buffers[loader->bufferCount++] = buf;
}

static void PushVisited(ModuleLoader* loader, const char* path)
{
    if (loader->visitedCount >= loader->visitedCap)
    {
        loader->visitedCap = loader->visitedCap ? loader->visitedCap * 2 : 8;
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

static void LoadInto(ModuleLoader* loader, const char* path)
{
    if (AlreadyVisited(loader, path))
    {
        return;
    }

    const char* pathKey = arena_strdup(loader->arena, path);
    PushVisited(loader, pathKey);

    size_t fileLen = 0;
    char* source = ReadFileAlloc(path, &fileLen);
    if (!source)
    {
        DiagErrorFmt(loader->diag, SRC_INVALID, "cannot open module '%s'", path);
        return;
    }

    PushBuffer(loader, source);

    if (loader->sourceCount >= loader->sourceCap)
    {
        loader->sourceCap = loader->sourceCap ? loader->sourceCap * 2 : 8;
        loader->sources = (SourceManager*)realloc(loader->sources, loader->sourceCap * sizeof(SourceManager));
    }

    uint16_t fileId = (uint16_t)loader->sourceCount;

    SourceManager* sm = &loader->sources[fileId];
    SourceManagerInit(sm);
    SourceManagerSetSource(sm, source, fileLen, pathKey);

    loader->sourceCount++;

    Lexer lex;
    LexerInit(&lex, sm->m_text, sm->m_textLen, loader->diag, fileId);

    Parser parser;
    ParserInit(&parser, &lex, loader->diag, loader->arena, pathKey);

    Module* fileMod = ParserParseModule(&parser);
    if (!fileMod)
    {
        return;
    }

    for (size_t i = 0; i < fileMod->imports.count; i++)
    {
        ImportDecl* imp = (ImportDecl*)VecGet(&fileMod->imports, i);
        char* childPath = ResolveImportPath(loader->arena, path, imp->importPath);
        LoadInto(loader, childPath);
    }

    AppendItems(loader->root, fileMod);
}

void ModuleLoaderInit(ModuleLoader* loader, Arena* arena, DiagnosticEngine* diag)
{
    *loader= (ModuleLoader){0};
    loader->arena = arena;
    loader->diag = diag;
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
    loader->root = AST_NEW(loader->arena, Module);
    loader->root->base.kind = NodeModule;
    loader->root->base.range = SRC_INVALID;
    loader->root->name = arena_strdup(loader->arena, mainPath);
    VecInit(&loader->root->structs);
    VecInit(&loader->root->handles);
    VecInit(&loader->root->functions);
    VecInit(&loader->root->globals);
    VecInit(&loader->root->imports);

    LoadInto(loader, mainPath);

    return loader->root;
}
