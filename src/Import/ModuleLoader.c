#include "Import/ModuleLoader.h"

#include "Core/Util.h"
#include "Lex/Lexer.h"
#include "Parse/Parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef STRATA_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static size_t GrowCap(size_t cap)
{
    return cap ? cap * 2 : 8;
}

// Canonical absolute spelling, so repeat imports dedup to one load.
static const char* CanonicalizePath(Arena* arena, const char* path)
{
#ifdef STRATA_PLATFORM_WINDOWS
    char buf[4096];
    char* rp = _fullpath(buf, path, sizeof(buf));
#else
    char* rp = realpath(path, NULL);
#endif

    if (rp)
    {
        const char* result = arena_strdup(arena, rp);

#ifndef STRATA_PLATFORM_WINDOWS
        free(rp);
#endif

        return result;
    }

    return arena_strdup(arena, path);
}

/* Dedup key for an on-disk module. Elsewhere this is the canonical path. On
   Windows `_fullpath` keeps the spelling it was given (`Util` vs `util`, or
   an 8.3 short name), and the file system is case-insensitive, so expand
   short names and case-fold to get one key per file. */
static const char* DiskModuleKey(Arena* arena, const char* canonPath)
{
#ifdef STRATA_PLATFORM_WINDOWS
    wchar_t wide[4096];
    wchar_t longPath[4096];

    int wideLen = MultiByteToWideChar(CP_ACP, 0, canonPath, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));

    if (wideLen > 0)
    {
        const wchar_t* folded = wide;
        DWORD longLen = GetLongPathNameW(wide, longPath, (DWORD)(sizeof(longPath) / sizeof(longPath[0])));

        if (longLen > 0 && longLen < (DWORD)(sizeof(longPath) / sizeof(longPath[0])))
        {
            folded = longPath;
        }

        size_t n = wcslen(folded);
        wchar_t* lower = (wchar_t*)arena_alloc(arena, (n + 1) * sizeof(wchar_t));
        memcpy(lower, folded, (n + 1) * sizeof(wchar_t));
        CharLowerBuffW(lower, (DWORD)n);

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, lower, -1, NULL, 0, NULL, NULL);

        if (utf8Len > 0)
        {
            char* key = (char*)arena_alloc(arena, (size_t)utf8Len);
            WideCharToMultiByte(CP_UTF8, 0, lower, -1, key, utf8Len, NULL, NULL);
            return key;
        }
    }
#endif

    return canonPath;
}

static char* ResolveImportPath(Arena* arena, const char* importerPath, const char* importPath)
{
    size_t impLen = strlen(importPath);

    bool hasExt = impLen >= 7 && strcmp(importPath + impLen - 7, ".strata") == 0;
    const char* ext = hasExt ? "" : ".strata";

    size_t dirLen = BasePathLength(importerPath);
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
        char** grown = (char**)realloc(loader->buffers, loader->bufferCap * sizeof(char*));
        if (!grown)
        {
            STRATA_OOM();
        }
        loader->buffers = grown;
    }
    loader->buffers[loader->bufferCount++] = buf;
}

static void PushVisited(ModuleLoader* loader, const char* path)
{
    if (loader->visitedCount >= loader->visitedCap)
    {
        loader->visitedCap = GrowCap(loader->visitedCap);
        const char** grownVisited = (const char**)realloc(loader->visited, loader->visitedCap * sizeof(const char*));
        if (!grownVisited)
        {
            STRATA_OOM();
        }
        loader->visited = grownVisited;
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

static bool RootHasHandle(const Module* root, const char* name)
{
    for (size_t i = 0; i < root->handles.count; i++)
    {
        HandleDecl* h = (HandleDecl*)VecGet(&root->handles, i);
        if (strcmp(h->name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool RootHasEnum(const Module* root, const char* name)
{
    for (size_t i = 0; i < root->enums.count; i++)
    {
        EnumDecl* e = (EnumDecl*)VecGet(&root->enums, i);
        if (strcmp(e->name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool RootHasGlobal(const Module* root, const char* name)
{
    for (size_t i = 0; i < root->globals.count; i++)
    {
        GlobalDecl* g = (GlobalDecl*)VecGet(&root->globals, i);
        if (strcmp(g->name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static void AppendItems(ModuleLoader* loader, const Module* src)
{
    Module* root = loader->root;
    DiagnosticEngine* diag = loader->diag;

    for (size_t i = 0; i < src->structs.count; i++)
    {
        StructDecl* s = (StructDecl*)VecGet(&src->structs, i);

        if (s->isExtern)
        {
            VecPush(&root->structs, s);
            continue;
        }

        bool hasAny = false;
        bool hasDefined = false;

        for (size_t j = 0; j < root->structs.count; j++)
        {
            StructDecl* r = (StructDecl*)VecGet(&root->structs, j);

            if (strcmp(r->name, s->name) != 0)
            {
                continue;
            }

            hasAny = true;

            if (!r->incomplete)
            {
                hasDefined = true;
            }
        }

        /* A forward declaration may coexist with (and be satisfied by) a
           definition in the other module - only two definitions conflict. */
        if (hasAny && !s->incomplete && hasDefined)
        {
            DiagErrorFmt(diag, s->base.range, "redefinition of struct '%s'", s->name);
            continue;
        }

        VecPush(&root->structs, s);
    }

    for (size_t i = 0; i < src->handles.count; i++)
    {
        HandleDecl* h = (HandleDecl*)VecGet(&src->handles, i);
        if (RootHasHandle(root, h->name))
        {
            DiagErrorFmt(diag, h->base.range, "redefinition of handle '%s'", h->name);
            continue;
        }
        VecPush(&root->handles, h);
    }

    for (size_t i = 0; i < src->enums.count; i++)
    {
        EnumDecl* e = (EnumDecl*)VecGet(&src->enums, i);
        if (RootHasEnum(root, e->name))
        {
            DiagErrorFmt(diag, e->base.range, "redefinition of enum '%s'", e->name);
            continue;
        }
        VecPush(&root->enums, e);
    }

    for (size_t i = 0; i < src->functions.count; i++)
    {
        VecPush(&root->functions, VecGet(&src->functions, i));
    }

    for (size_t i = 0; i < src->globals.count; i++)
    {
        GlobalDecl* g = (GlobalDecl*)VecGet(&src->globals, i);
        if (RootHasGlobal(root, g->name))
        {
            DiagErrorFmt(diag, g->base.range, "redefinition of global '%s'", g->name);
            continue;
        }
        VecPush(&root->globals, g);
    }

    /* Impl blocks merge wholesale; duplicate qualified method symbols across
       impls are diagnosed in sema (duplicate signature / extern overload). */
    for (size_t i = 0; i < src->impls.count; i++)
    {
        VecPush(&root->impls, VecGet(&src->impls, i));
    }
}

// Forward declarations.
static void LoadStrataModule(ModuleLoader* loader, const char* key, const char* name, const char* text, size_t textLen,
                             bool textOwned);
static void ResolveImport(ModuleLoader* loader, const char* importerName, const char* importPath);

/* `key` identifies the module for dedup (see DiskModuleKey); `name` is the
   canonical name used for diagnostics and passed to the resolver as the
   importer. */
static void LoadStrataModule(ModuleLoader* loader, const char* key, const char* name, const char* text, size_t textLen,
                             bool textOwned)
{
    // Take ownership of caller-owned text before any early return so it can never leak.
    if (textOwned)
    {
        PushBuffer(loader, (char*)text);
    }

    if (AlreadyVisited(loader, key))
    {
        return;
    }

    PushVisited(loader, arena_strdup(loader->arena, key));

    const char* nameKey = arena_strdup(loader->arena, name);

    if (loader->sourceCount >= loader->sourceCap)
    {
        loader->sourceCap = GrowCap(loader->sourceCap);
        SourceManager* grownSources
            = (SourceManager*)realloc(loader->sources, loader->sourceCap * sizeof(SourceManager));
        if (!grownSources)
        {
            STRATA_OOM();
        }
        loader->sources = grownSources;
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

    AppendItems(loader, fileMod);
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

        /* The canonical name is the module's identity: two imports that
           resolve to the same name are one module. Falling back to the import
           spelling would silently merge distinct modules that happen to be
           imported as the same path from different importers. */
        if (!resolved.name || !resolved.name[0])
        {
            DiagErrorFmt(loader->diag, SRC_INVALID,
                         "import resolver did not set a canonical module name for import '%s'", importPath);
            return;
        }

        // `name` is copied inside LoadStrataModule; `text` is borrowed for the compile (host owns it).
        LoadStrataModule(loader, resolved.name, resolved.name, resolved.text, resolved.length, false);
        return;
    }

    char* childPath = ResolveImportPath(loader->arena, importerName, importPath);
    const char* canonPath = CanonicalizePath(loader->arena, childPath);
    const char* key = DiskModuleKey(loader->arena, canonPath);

    // Skip re-reading disk files we've already loaded (canonicalized so that
    // e.g. "foo" and "foo.strata" or "./foo.strata" resolve to the same key).
    if (AlreadyVisited(loader, key))
    {
        return;
    }

    size_t fileLen = 0;
    char* source = ReadWholeFile(canonPath, &fileLen);
    if (!source)
    {
        DiagErrorFmt(loader->diag, SRC_INVALID, "cannot open module '%s'", canonPath);
        return;
    }

    LoadStrataModule(loader, key, canonPath, source, fileLen, true);
}

static Module* NewRootModule(Arena* arena, const char* name)
{
    Module* root = AST_NEW(arena, Module);
    root->base.kind = NodeModule;
    root->base.range = SRC_INVALID;
    root->name = arena_strdup(arena, name);
    VecInit(&root->structs);
    VecInit(&root->handles);
    VecInit(&root->enums);
    VecInit(&root->functions);
    VecInit(&root->globals);
    VecInit(&root->imports);
    VecInit(&root->impls);

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

    free((void*)loader->buffers);
    free((void*)loader->visited);

    *loader = (ModuleLoader){0};
}

Module* ModuleLoaderLoad(ModuleLoader* loader, const char* mainPath)
{
    loader->root = NewRootModule(loader->arena, mainPath);

    // Canonicalize so an indirect import cycle that reaches this same file is
    // recognized as already visited.
    const char* canonMain = CanonicalizePath(loader->arena, mainPath);

    // The main file is supplied by the caller from disk; only its imports go through the resolver (if set).
    size_t fileLen = 0;
    char* source = ReadWholeFile(canonMain, &fileLen);
    if (!source)
    {
        DiagErrorFmt(loader->diag, SRC_INVALID, "cannot open module '%s'", mainPath);
        return loader->root;
    }

    LoadStrataModule(loader, DiskModuleKey(loader->arena, canonMain), canonMain, source, fileLen, true);

    return loader->root;
}

Module* ModuleLoaderLoadSource(ModuleLoader* loader, const char* name, const char* text, size_t textLen)
{
    loader->root = NewRootModule(loader->arena, name ? name : "<string>");

    // The main source is the caller's string (borrowed). Imports require a resolver (no filesystem context).
    LoadStrataModule(loader, loader->root->name, loader->root->name, text, textLen, false);

    return loader->root;
}
