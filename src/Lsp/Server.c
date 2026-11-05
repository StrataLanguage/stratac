#include "Lsp/Server.h"

#include "Embed/EmbedInternal.h"
#include "Lsp/Json.h"
#include "Lsp/Transport.h"
#include "Lsp/Uri.h"

#include <stdlib.h>
#include <string.h>

// JSON-RPC error codes from the specification.
#define LSP_ERR_PARSE (-32700)
#define LSP_ERR_INVALID_REQUEST (-32600)
#define LSP_ERR_METHOD_NOT_FOUND (-32601)

typedef struct
{
    char* uri;       // as the client sent it; diagnostics for the document are published under this
    char* path;      // file path used as the module name, or NULL when the URI is not a file
    char* canonPath; // the loader's canonical spelling of `path` (see ModuleLoaderCanonicalPath), or NULL
    char* text;      // full buffer contents
    size_t textLen;
    char** published; // canonical paths of other files that last received diagnostics from this document
    size_t publishedCount;
    char** deps; // canonical paths of every file the last analysis loaded through imports
    size_t depCount;
} LspDoc;

struct LspServer
{
    LspDoc** docs;
    size_t docCount;
    size_t docCap;

    char** importDirs;
    size_t importDirCount;
    size_t importDirCap;

    bool shutdownRequested;
    bool exitRequested;
};

static const char kInitializeResult[]
    = "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}}},"
      "\"serverInfo\":{\"name\":\"strata-lsp\",\"version\":\"0.1.0\"}}";

// -- Output

static void Send(Sb* sb)
{
    LspWriteMessage(stdout, sb->data ? sb->data : "", sb->len);
    SbFree(sb);
}

static void SendResult(const JValue* id, const char* resultJson)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    JsonPutValue(&sb, id);
    SbPuts(&sb, ",\"result\":");
    SbPuts(&sb, resultJson);
    SbPutc(&sb, '}');
    Send(&sb);
}

static void SendError(const JValue* id, int code, const char* message)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    JsonPutValue(&sb, id);
    SbPrintf(&sb, ",\"error\":{\"code\":%d,\"message\":", code);
    JsonPutString(&sb, message, strlen(message));
    SbPuts(&sb, "}}");
    Send(&sb);
}

// Publishes diagnostics for `uri`. `items` holds comma-separated diagnostic objects, or NULL for none.
static void SendPublish(const char* uri, const Sb* items)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    JsonPutString(&sb, uri, strlen(uri));
    SbPuts(&sb, ",\"diagnostics\":[");
    if (items && items->len > 0)
    {
        SbPutn(&sb, items->data, items->len);
    }
    SbPuts(&sb, "]}}");
    Send(&sb);
}

// -- Diagnostics

// Writes an LSP position for a byte offset. Columns count UTF-16 code units, the protocol default.
static void AppendPosition(Sb* sb, const SourceManager* sm, uint32_t offset)
{
    if (offset > sm->textLen)
    {
        offset = (uint32_t)sm->textLen;
    }

    LineCol lc = SourceManagerLineCol(sm, offset);
    uint32_t line = lc.line > 0 ? lc.line - 1 : 0;
    uint32_t lineStart = offset - (lc.column > 0 ? lc.column - 1 : 0);

    uint32_t units = 0;
    for (uint32_t i = lineStart; i < offset; i++)
    {
        unsigned char b = (unsigned char)sm->text[i];
        if ((b & 0xC0) == 0x80)
        {
            continue; // UTF-8 continuation byte
        }
        units += b >= 0xF0 ? 2 : 1; // four-byte sequences need a surrogate pair
    }

    SbPrintf(sb, "{\"line\":%u,\"character\":%u}", line, units);
}

static void AppendDiagnostic(Sb* sb, const SourceManager* sm, const Diagnostic* d)
{
    uint32_t start = d->range.start;
    uint32_t end = SourceRangeEnd(d->range);
    if (end < start)
    {
        end = start;
    }

    int severity = d->severity == SevError ? 1 : d->severity == SevWarning ? 2 : 3;
    const char* message = d->message ? d->message : "";

    SbPuts(sb, "{\"range\":{\"start\":");
    AppendPosition(sb, sm, start);
    SbPuts(sb, ",\"end\":");
    AppendPosition(sb, sm, end);
    SbPrintf(sb, "},\"severity\":%d,\"source\":\"strata\",\"message\":", severity);
    JsonPutString(sb, message, strlen(message));
    SbPutc(sb, '}');
}

static bool ContainsString(char* const* list, size_t count, const char* str)
{
    for (size_t i = 0; i < count; i++)
    {
        if (strcmp(list[i], str) == 0)
        {
            return true;
        }
    }
    return false;
}

static void FreeStringList(char** list, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        free(list[i]);
    }
    free(list);
}

// The open document backed by the file at `canonPath`, if any.
static LspDoc* FindDocByCanonPath(LspServer* s, const char* canonPath)
{
    for (size_t i = 0; i < s->docCount; i++)
    {
        if (s->docs[i]->canonPath && strcmp(s->docs[i]->canonPath, canonPath) == 0)
        {
            return s->docs[i];
        }
    }
    return NULL;
}

// Loader overlay: imports of an open file use its editor buffer, not the copy on disk.
static const char* OpenBufferOverlay(void* userData, const char* canonPath, size_t* lenOut)
{
    LspDoc* doc = FindDocByCanonPath((LspServer*)userData, canonPath);
    if (!doc)
    {
        return NULL;
    }
    *lenOut = doc->textLen;
    return doc->text;
}

// Publishes for another file that this document's analysis reached. An open file is skipped: its own analysis
// owns its diagnostics, and publishing here would overwrite them.
static void PublishForImported(LspServer* s, Arena* arena, const char* canonPath, const Sb* items)
{
    if (FindDocByCanonPath(s, canonPath))
    {
        return;
    }
    SendPublish(PathToUri(arena, canonPath), items);
}

// Runs parse and semantic analysis over the document's current buffer and publishes the result for every
// file that has diagnostics. Imported files that are open are read from their buffers; the rest from disk.
static void AnalyzeDoc(LspServer* s, LspDoc* doc)
{
    Arena arena;
    DiagnosticEngine diag;
    ModuleLoader loader;

    ArenaInit(&arena, 0);
    DiagnosticEngineInit(&diag);
    ModuleLoaderInit(&loader, &arena, &diag);
    ModuleLoaderSetImportDirs(&loader, (const char* const*)s->importDirs, s->importDirCount);
    ModuleLoaderSetOverlay(&loader, OpenBufferOverlay, s);

    Module* mod = doc->path ? ModuleLoaderLoadBuffer(&loader, doc->path, doc->text, doc->textLen)
                            : ModuleLoaderLoadSource(&loader, doc->uri, doc->text, doc->textLen);
    AnalyzeModule(mod, &diag, &arena);

    size_t fileCount = loader.sourceCount;
    Sb* perFile = (Sb*)calloc(fileCount ? fileCount : 1, sizeof(Sb));
    size_t* perFileCount = (size_t*)calloc(fileCount ? fileCount : 1, sizeof(size_t));
    char** newPublished = (char**)calloc(fileCount ? fileCount : 1, sizeof(char*));
    size_t newPublishedCount = 0;

    // Source 0 is this document; the rest were loaded through imports.
    FreeStringList(doc->deps, doc->depCount);
    doc->deps = (char**)calloc(fileCount ? fileCount : 1, sizeof(char*));
    doc->depCount = 0;
    for (size_t f = 1; f < fileCount; f++)
    {
        doc->deps[doc->depCount++] = DupString(loader.sources[f].name);
    }

    if (fileCount > 0)
    {
        // Group diagnostics by the file they point into. Anything without a known file goes to the open document.
        for (size_t i = 0; i < diag.count; i++)
        {
            const Diagnostic* d = &diag.diagnostics[i];
            size_t f = d->range.fileId;
            if (f >= fileCount)
            {
                f = 0;
            }

            if (perFileCount[f]++ > 0)
            {
                SbPutc(&perFile[f], ',');
            }
            AppendDiagnostic(&perFile[f], &loader.sources[f], d);
        }

        // The open document is always published, so errors that were fixed get cleared.
        SendPublish(doc->uri, &perFile[0]);

        for (size_t f = 1; f < fileCount; f++)
        {
            if (perFileCount[f] == 0)
            {
                continue;
            }

            const char* path = loader.sources[f].name;
            PublishForImported(s, &arena, path, &perFile[f]);
            newPublished[newPublishedCount++] = DupString(path);
        }

        // Other files that had diagnostics last time and have none now get an empty list.
        for (size_t i = 0; i < doc->publishedCount; i++)
        {
            if (!ContainsString(newPublished, newPublishedCount, doc->published[i]))
            {
                PublishForImported(s, &arena, doc->published[i], NULL);
            }
        }
    }

    FreeStringList(doc->published, doc->publishedCount);
    doc->published = newPublished;
    doc->publishedCount = newPublishedCount;

    for (size_t f = 0; f < fileCount; f++)
    {
        SbFree(&perFile[f]);
    }
    free(perFile);
    free(perFileCount);

    TeardownCompile(mod, &loader, &diag, &arena);
}

// -- Documents

static char* DupBytes(const char* s, size_t n)
{
    char* d = (char*)malloc(n + 1);
    if (!d)
    {
        STRATA_OOM();
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char* UriToPathOwned(const char* uri)
{
    Arena a;
    ArenaInit(&a, 0);
    char* path = UriToPath(&a, uri);
    char* owned = path ? DupString(path) : NULL;
    ArenaFree(&a);
    return owned;
}

static LspDoc* FindDoc(LspServer* s, const char* uri)
{
    for (size_t i = 0; i < s->docCount; i++)
    {
        if (strcmp(s->docs[i]->uri, uri) == 0)
        {
            return s->docs[i];
        }
    }
    return NULL;
}

static LspDoc* AddDoc(LspServer* s, const char* uri)
{
    if (s->docCount >= s->docCap)
    {
        s->docCap = s->docCap ? s->docCap * 2 : 8;
        s->docs = (LspDoc**)realloc(s->docs, s->docCap * sizeof(LspDoc*));
        if (!s->docs)
        {
            STRATA_OOM();
        }
    }

    LspDoc* doc = (LspDoc*)calloc(1, sizeof(LspDoc));
    if (!doc)
    {
        STRATA_OOM();
    }
    doc->uri = DupString(uri);
    doc->path = UriToPathOwned(uri);
    if (doc->path)
    {
        Arena a;
        ArenaInit(&a, 0);
        doc->canonPath = DupString(ModuleLoaderCanonicalPath(&a, doc->path));
        ArenaFree(&a);
    }

    s->docs[s->docCount++] = doc;
    return doc;
}

// Re-analyzes the other open documents whose last analysis loaded `changed`, so they see its new buffer
// (or, once it closes, its contents on disk).
static void AnalyzeDependents(LspServer* s, const LspDoc* changed, const char* changedCanonPath)
{
    if (!changedCanonPath)
    {
        return;
    }

    for (size_t i = 0; i < s->docCount; i++)
    {
        LspDoc* other = s->docs[i];
        if (other != changed && ContainsString(other->deps, other->depCount, changedCanonPath))
        {
            AnalyzeDoc(s, other);
        }
    }
}

static void SetDocText(LspDoc* doc, const char* text, size_t len)
{
    free(doc->text);
    doc->text = DupBytes(text, len);
    doc->textLen = len;
}

static void FreeDoc(LspDoc* doc)
{
    free(doc->uri);
    free(doc->path);
    free(doc->canonPath);
    free(doc->text);
    FreeStringList(doc->published, doc->publishedCount);
    FreeStringList(doc->deps, doc->depCount);
    free(doc);
}

static void CloseDoc(LspServer* s, LspDoc* doc)
{
    // Clear what this document published, so no stale errors stay in the editor. Files that are still open
    // keep theirs.
    Arena arena;
    ArenaInit(&arena, 0);
    SendPublish(doc->uri, NULL);
    for (size_t i = 0; i < doc->publishedCount; i++)
    {
        PublishForImported(s, &arena, doc->published[i], NULL);
    }
    ArenaFree(&arena);

    for (size_t i = 0; i < s->docCount; i++)
    {
        if (s->docs[i] == doc)
        {
            s->docs[i] = s->docs[--s->docCount];
            break;
        }
    }

    // Importers fall back to the file on disk now that its buffer is gone.
    char* canonPath = doc->canonPath;
    doc->canonPath = NULL;
    FreeDoc(doc);
    AnalyzeDependents(s, NULL, canonPath);
    free(canonPath);
}

// -- Field helpers

static const JValue* StrValue(const JValue* obj, const char* key)
{
    const JValue* v = JsonGet(obj, key);
    return (v && v->type == JString) ? v : NULL;
}

// -- Public API

LspServer* LspServerCreate(void)
{
    return (LspServer*)calloc(1, sizeof(LspServer));
}

void LspServerDestroy(LspServer* s)
{
    if (!s)
    {
        return;
    }

    // Shutdown frees silently: the client has already been told what to clear.
    while (s->docCount > 0)
    {
        FreeDoc(s->docs[--s->docCount]);
    }
    free(s->docs);

    FreeStringList(s->importDirs, s->importDirCount);
    free(s);
}

void LspServerAddImportDir(LspServer* s, const char* dir)
{
    if (s->importDirCount >= s->importDirCap)
    {
        s->importDirCap = s->importDirCap ? s->importDirCap * 2 : 4;
        s->importDirs = (char**)realloc(s->importDirs, s->importDirCap * sizeof(char*));
        if (!s->importDirs)
        {
            STRATA_OOM();
        }
    }
    s->importDirs[s->importDirCount++] = DupString(dir);
}

int LspServerExitCode(const LspServer* s)
{
    return s->shutdownRequested ? 0 : 1;
}

bool LspServerHandleMessage(LspServer* s, const char* body, size_t len)
{
    Arena arena;
    ArenaInit(&arena, 0);

    JValue* root = NULL;
    if (!JsonParse(&arena, body, len, &root))
    {
        SendError(NULL, LSP_ERR_PARSE, "parse error");
        ArenaFree(&arena);
        return true;
    }

    if (root->type != JObject)
    {
        SendError(NULL, LSP_ERR_INVALID_REQUEST, "request must be an object");
        ArenaFree(&arena);
        return true;
    }

    // Requests carry an id and expect a response; notifications do not.
    const JValue* id = JsonGet(root, "id");
    const JValue* methodVal = StrValue(root, "method");
    const JValue* params = JsonGet(root, "params");

    // A message without a method is a response to a request this server never sent; nothing to do.
    if (!methodVal)
    {
        ArenaFree(&arena);
        return true;
    }

    const char* method = methodVal->str;
    bool keepRunning = true;

    if (strcmp(method, "initialize") == 0)
    {
        const JValue* options = JsonGet(params, "initializationOptions");
        const JValue* dirs = JsonGet(options, "importDirs");
        if (dirs && dirs->type == JArray)
        {
            for (size_t i = 0; i < dirs->count; i++)
            {
                if (dirs->items[i]->type == JString)
                {
                    LspServerAddImportDir(s, dirs->items[i]->str);
                }
            }
        }

        if (id)
        {
            SendResult(id, kInitializeResult);
        }
    }
    else if (strcmp(method, "initialized") == 0)
    {
        // Nothing to do: the client has finished the handshake.
    }
    else if (strcmp(method, "shutdown") == 0)
    {
        s->shutdownRequested = true;
        if (id)
        {
            SendResult(id, "null");
        }
    }
    else if (strcmp(method, "exit") == 0)
    {
        s->exitRequested = true;
        keepRunning = false;
    }
    else if (strcmp(method, "textDocument/didOpen") == 0)
    {
        const JValue* td = JsonGet(params, "textDocument");
        const JValue* uri = StrValue(td, "uri");
        const JValue* text = StrValue(td, "text");
        if (uri && text)
        {
            LspDoc* doc = FindDoc(s, uri->str);
            if (!doc)
            {
                doc = AddDoc(s, uri->str);
            }
            SetDocText(doc, text->str, text->strLen);
            AnalyzeDoc(s, doc);
            AnalyzeDependents(s, doc, doc->canonPath);
        }
    }
    else if (strcmp(method, "textDocument/didChange") == 0)
    {
        // Full-text sync: the last change holds the whole buffer.
        const JValue* td = JsonGet(params, "textDocument");
        const JValue* uri = StrValue(td, "uri");
        const JValue* changes = JsonGet(params, "contentChanges");
        if (uri && changes && changes->type == JArray && changes->count > 0)
        {
            const JValue* last = changes->items[changes->count - 1];
            const JValue* text = StrValue(last, "text");
            if (text)
            {
                LspDoc* doc = FindDoc(s, uri->str);
                if (!doc)
                {
                    doc = AddDoc(s, uri->str);
                }
                SetDocText(doc, text->str, text->strLen);
                AnalyzeDoc(s, doc);
                AnalyzeDependents(s, doc, doc->canonPath);
            }
        }
    }
    else if (strcmp(method, "textDocument/didSave") == 0)
    {
        // Changes are analyzed as they arrive, so saving needs no work.
    }
    else if (strcmp(method, "textDocument/didClose") == 0)
    {
        const JValue* uri = StrValue(JsonGet(params, "textDocument"), "uri");
        LspDoc* doc = uri ? FindDoc(s, uri->str) : NULL;
        if (doc)
        {
            CloseDoc(s, doc);
        }
    }
    else if (id)
    {
        SendError(id, LSP_ERR_METHOD_NOT_FOUND, "method not found");
    }

    ArenaFree(&arena);
    return keepRunning;
}
