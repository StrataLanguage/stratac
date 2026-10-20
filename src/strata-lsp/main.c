/* strata-lsp -- a Language Server Protocol server for Strata.

   Speaks JSON-RPC over stdio. Each open document is compiled through the real front end
   (ModuleLoader + ResolveOverloads, no codegen) a short while after the last edit, and the
   resulting diagnostics are published back to the editor. */

#include "Json.h"

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Core/Util.h"
#include "Import/ModuleLoader.h"
#include "Sema/ResolveOverloads.h"

#include <ctype.h>
#include <stdlib.h>

#if defined(STRATA_PLATFORM_WINDOWS)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#define STRATA_LSP_VERSION "0.1.0"

// Quiet time after the last message before dirty documents are recompiled.
#define DEBOUNCE_MS 150

// JSON-RPC error codes.
#define RPC_INVALID_REQUEST -32600
#define RPC_METHOD_NOT_FOUND -32601
#define RPC_SERVER_NOT_INITIALIZED -32002

typedef enum {
    EncodingUtf16,
    EncodingUtf8,
} PositionEncoding;

typedef struct {
    char* uri;  // as the client sent it; diagnostics are published back to this exact string
    char* name; // module name handed to the loader (a canonical path for file:// documents)
    char* text;
    size_t len;
    bool dirty;
} Document;

typedef struct {
    bool initialized;
    bool shutdownRequested;
    PositionEncoding encoding;

    char** importDirs;
    size_t importDirCount;

    Document* docs;
    size_t docCount;
    size_t docCap;
} Server;

static Server g_server;

// -- Logging (stderr shows up in the editor's output panel)

static void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("strata-lsp: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

// -- Input

typedef struct {
    char* data;
    size_t len;
    size_t pos;
    size_t cap;
} InputBuffer;

static InputBuffer g_input;

static long ReadStdin(char* buf, size_t cap)
{
#if defined(STRATA_PLATFORM_WINDOWS)
    return (long)_read(0, buf, (unsigned)(cap > 0x40000000 ? 0x40000000 : cap));
#else
    return (long)read(0, buf, cap);
#endif
}

// Appends whatever stdin has to the buffer (blocking). False on EOF or error.
static bool FillInput(void)
{
    if (g_input.pos > 0)
    {
        memmove(g_input.data, g_input.data + g_input.pos, g_input.len - g_input.pos);
        g_input.len -= g_input.pos;
        g_input.pos = 0;
    }

    if (g_input.cap - g_input.len < 4096)
    {
        size_t cap = g_input.cap ? g_input.cap * 2 : 65536;
        char* grown = (char*)realloc(g_input.data, cap);
        if (!grown)
        {
            STRATA_OOM();
        }
        g_input.data = grown;
        g_input.cap = cap;
    }

    long n = ReadStdin(g_input.data + g_input.len, g_input.cap - g_input.len);
    if (n <= 0)
    {
        return false;
    }

    g_input.len += (size_t)n;
    return true;
}

// True when input is already buffered or arrives within `timeoutMs`.
static bool WaitForInput(int timeoutMs)
{
    if (g_input.pos < g_input.len)
    {
        return true;
    }

#if defined(STRATA_PLATFORM_WINDOWS)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (int waited = 0;; waited += 10)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(in, NULL, 0, NULL, &avail, NULL))
        {
            return true; // not a pipe (or broken): let the blocking read sort it out
        }
        if (avail > 0)
        {
            return true;
        }
        if (waited >= timeoutMs)
        {
            return false;
        }
        Sleep(10);
    }
#else
    struct pollfd pfd = {0, POLLIN, 0};
    return poll(&pfd, 1, timeoutMs) != 0;
#endif
}

// Reads one header line (without its CRLF) into `line`. False on EOF.
static bool ReadHeaderLine(char* line, size_t cap)
{
    for (;;)
    {
        for (size_t i = g_input.pos; i < g_input.len; i++)
        {
            if (g_input.data[i] != '\n')
            {
                continue;
            }

            size_t n = i - g_input.pos;
            if (n > 0 && g_input.data[i - 1] == '\r')
            {
                n--;
            }
            if (n >= cap)
            {
                n = cap - 1;
            }

            memcpy(line, g_input.data + g_input.pos, n);
            line[n] = '\0';
            g_input.pos = i + 1;
            return true;
        }

        if (!FillInput())
        {
            return false;
        }
    }
}

// One message body (malloc'd, NUL-terminated), or NULL at end of input.
static char* ReadMessage(size_t* outLen)
{
    size_t contentLength = 0;
    bool haveLength = false;
    char line[512];

    for (;;)
    {
        if (!ReadHeaderLine(line, sizeof(line)))
        {
            return NULL;
        }

        if (line[0] == '\0')
        {
            if (haveLength)
            {
                break;
            }
            continue; // stray blank line before the headers
        }

        static const char kLength[] = "content-length:";
        size_t n = sizeof(kLength) - 1;
        bool match = strlen(line) > n;
        for (size_t i = 0; match && i < n; i++)
        {
            match = tolower((unsigned char)line[i]) == kLength[i];
        }

        if (match)
        {
            contentLength = (size_t)strtoull(line + n, NULL, 10);
            haveLength = true;
        }
    }

    while (g_input.len - g_input.pos < contentLength)
    {
        if (!FillInput())
        {
            return NULL;
        }
    }

    char* body = (char*)malloc(contentLength + 1);
    if (!body)
    {
        STRATA_OOM();
    }

    memcpy(body, g_input.data + g_input.pos, contentLength);
    body[contentLength] = '\0';
    g_input.pos += contentLength;

    *outLen = contentLength;
    return body;
}

// -- Output

static void SendRpc(const Sb* sb)
{
    fprintf(stdout, "Content-Length: %lu\r\n\r\n", (unsigned long)sb->len);
    fwrite(sb->data, 1, sb->len, stdout);
    fflush(stdout);
}

static void SendResult(Str id, const char* resultJson)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    SbPutn(&sb, id.data, id.len);
    SbPuts(&sb, ",\"result\":");
    SbPuts(&sb, resultJson);
    SbPutc(&sb, '}');
    SendRpc(&sb);
    SbFree(&sb);
}

static void SendError(Str id, int code, const char* message)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id.len)
    {
        SbPutn(&sb, id.data, id.len);
    }
    else
    {
        SbPuts(&sb, "null");
    }
    SbPrintf(&sb, ",\"error\":{\"code\":%d,\"message\":", code);
    JsonWriteString(&sb, message, strlen(message));
    SbPuts(&sb, "}}");
    SendRpc(&sb);
    SbFree(&sb);
}

// -- URIs

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The local path of a file:// URI (malloc'd), or NULL for any other scheme.
static char* UriToPath(const char* uri)
{
    if (strncmp(uri, "file://", 7) != 0)
    {
        return NULL;
    }

    const char* p = uri + 7;
    if (*p != '/')
    {
        p = strchr(p, '/'); // skip an authority such as `localhost`
        if (!p)
        {
            return NULL;
        }
    }

    char* path = (char*)malloc(strlen(p) + 1);
    if (!path)
    {
        STRATA_OOM();
    }

    size_t n = 0;
    for (; *p; p++)
    {
        int hi, lo;
        if (*p == '%' && (hi = HexValue(p[1])) >= 0 && (lo = HexValue(p[2])) >= 0)
        {
            path[n++] = (char)((hi << 4) | lo);
            p += 2;
        }
        else
        {
            path[n++] = *p;
        }
    }
    path[n] = '\0';

#if defined(STRATA_PLATFORM_WINDOWS)
    // `/c:/dir/file` -> `c:/dir/file`
    if (n >= 3 && path[0] == '/' && isalpha((unsigned char)path[1]) && path[2] == ':')
    {
        memmove(path, path + 1, n);
    }
#endif

    return path;
}

static void WritePathUri(Sb* sb, const char* path)
{
    static const char kHex[] = "0123456789ABCDEF";

    SbPuts(sb, "file://");
    if (path[0] != '/')
    {
        SbPutc(sb, '/'); // Windows drive paths
    }

    for (const unsigned char* p = (const unsigned char*)path; *p; p++)
    {
        unsigned char c = *p == '\\' ? '/' : *p;
        if (isalnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~' || c == ':')
        {
            SbPutc(sb, (char)c);
        }
        else
        {
            SbPutc(sb, '%');
            SbPutc(sb, kHex[c >> 4]);
            SbPutc(sb, kHex[c & 0xF]);
        }
    }
}

// Module name for a document: its canonical path when it lives on disk (so relative
// `import`s resolve next to it and an import cycle back to it is recognised), else the URI.
static char* DocumentModuleName(const char* uri)
{
    char* path = UriToPath(uri);
    if (!path)
    {
        return DupString(uri);
    }

#if defined(STRATA_PLATFORM_WINDOWS)
    char buf[4096];
    if (_fullpath(buf, path, sizeof(buf)))
    {
        free(path);
        path = DupString(buf);
    }
    for (char* p = path; *p; p++)
    {
        *p = (char)tolower((unsigned char)*p); // the loader case-folds disk module keys on Windows
    }
#else
    char* canon = realpath(path, NULL);
    if (canon)
    {
        free(path);
        path = canon;
    }
#endif

    return path;
}

// -- Documents

static Document* FindDocument(const char* uri)
{
    for (size_t i = 0; i < g_server.docCount; i++)
    {
        if (strcmp(g_server.docs[i].uri, uri) == 0)
        {
            return &g_server.docs[i];
        }
    }

    return NULL;
}

static void SetDocumentText(Document* doc, const JsonValue* text)
{
    free(doc->text);
    doc->text = (char*)malloc(text->string.len + 1);
    if (!doc->text)
    {
        STRATA_OOM();
    }

    memcpy(doc->text, text->string.data, text->string.len);
    doc->text[text->string.len] = '\0';
    doc->len = text->string.len;
    doc->dirty = true;
}

static Document* OpenDocument(const char* uri)
{
    Document* doc = FindDocument(uri);
    if (doc)
    {
        return doc;
    }

    if (g_server.docCount == g_server.docCap)
    {
        g_server.docCap = g_server.docCap ? g_server.docCap * 2 : 8;
        Document* grown = (Document*)realloc(g_server.docs, g_server.docCap * sizeof(Document));
        if (!grown)
        {
            STRATA_OOM();
        }
        g_server.docs = grown;
    }

    doc = &g_server.docs[g_server.docCount++];
    memset(doc, 0, sizeof(*doc));
    doc->uri = DupString(uri);
    doc->name = DocumentModuleName(uri);
    return doc;
}

static void CloseDocument(const char* uri)
{
    Document* doc = FindDocument(uri);
    if (!doc)
    {
        return;
    }

    free(doc->uri);
    free(doc->name);
    free(doc->text);

    size_t index = (size_t)(doc - g_server.docs);
    memmove(doc, doc + 1, (g_server.docCount - index - 1) * sizeof(Document));
    g_server.docCount--;
}

// -- Positions

typedef struct {
    uint32_t line;      // 0-based
    uint32_t character; // 0-based, in the negotiated encoding's code units
} LspPosition;

static LspPosition OffsetToPosition(const char* text, size_t len, uint32_t offset)
{
    if (offset > len)
    {
        offset = (uint32_t)len;
    }

    uint32_t line = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < offset; i++)
    {
        if (text[i] == '\n')
        {
            line++;
            lineStart = i + 1;
        }
    }

    uint32_t character = 0;
    for (size_t i = lineStart; i < offset; i++)
    {
        unsigned char c = (unsigned char)text[i];

        if (g_server.encoding == EncodingUtf8)
        {
            character++;
        }
        else if (c < 0x80 || (c >= 0xC0 && c < 0xF0))
        {
            character++; // ASCII, or the lead byte of a code point in the BMP
        }
        else if (c >= 0xF0)
        {
            character += 2; // astral code point: a UTF-16 surrogate pair
        }
        // continuation bytes add nothing
    }

    return (LspPosition){line, character};
}

static void WriteRange(Sb* sb, const char* text, size_t len, SourceRange range)
{
    LspPosition start = OffsetToPosition(text, len, range.start);
    LspPosition end = OffsetToPosition(text, len, SourceRangeEnd(range));

    SbPrintf(sb, "{\"start\":{\"line\":%u,\"character\":%u},\"end\":{\"line\":%u,\"character\":%u}}",
             start.line, start.character, end.line, end.character);
}

// -- Diagnostics

static int LspSeverity(DiagSeverity severity)
{
    switch (severity)
    {
    case SevError:
        return 1;
    case SevWarning:
        return 2;
    case SevNote:
        return 3;
    }

    return 1;
}

static void WriteDiagnostic(Sb* sb, const Diagnostic* d, const Document* doc, const ModuleLoader* loader)
{
    static const SourceRange kTop = {0, 0, 0};

    const SourceManager* file = NULL;
    if (SourceRangeValid(d->range) && d->range.fileId < loader->sourceCount)
    {
        file = &loader->sources[d->range.fileId];
    }

    SbPuts(sb, "{\"source\":\"strata\",");
    SbPrintf(sb, "\"severity\":%d,\"range\":", LspSeverity(d->severity));

    if (file && d->range.fileId == 0)
    {
        WriteRange(sb, doc->text, doc->len, d->range);
        SbPuts(sb, ",\"message\":");
        JsonWriteString(sb, d->message, strlen(d->message));
    }
    else if (file)
    {
        // The problem is inside an imported module: flag it at the top of this document and
        // link to the real location.
        LspPosition at = OffsetToPosition(file->m_text, file->m_textLen, d->range.start);
        const char* base = file->m_name + BasePathLength(file->m_name);
        base += (*base == '/' || *base == '\\') ? 1 : 0;

        WriteRange(sb, doc->text, doc->len, kTop);
        SbPuts(sb, ",\"message\":");
        char* message = arena_format(scratch_arena(), "in imported module %s:%u:%u: %s", base, at.line + 1,
                                     at.character + 1, d->message);
        JsonWriteString(sb, message, strlen(message));

        SbPuts(sb, ",\"relatedInformation\":[{\"location\":{\"uri\":\"");
        WritePathUri(sb, file->m_name);
        SbPuts(sb, "\",\"range\":");
        WriteRange(sb, file->m_text, file->m_textLen, d->range);
        SbPuts(sb, "},\"message\":");
        JsonWriteString(sb, d->message, strlen(d->message));
        SbPuts(sb, "}]");
    }
    else
    {
        WriteRange(sb, doc->text, doc->len, kTop);
        SbPuts(sb, ",\"message\":");
        JsonWriteString(sb, d->message, strlen(d->message));
    }

    SbPutc(sb, '}');
}

static void PublishDiagnostics(const char* uri, const char* diagnosticsJson)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    JsonWriteString(&sb, uri, strlen(uri));
    SbPuts(&sb, ",\"diagnostics\":");
    SbPuts(&sb, diagnosticsJson);
    SbPuts(&sb, "}}");
    SendRpc(&sb);
    SbFree(&sb);
}

// Runs the front end over one document and publishes what it reports.
static void AnalyzeDocument(Document* doc)
{
    Arena arena;
    DiagnosticEngine diag;
    ModuleLoader loader;

    arena_init(&arena, 0);
    DiagnosticEngineInit(&diag);
    ModuleLoaderInit(&loader, &arena, &diag);
    ModuleLoaderSetImportDirs(&loader, (const char* const*)g_server.importDirs, g_server.importDirCount);

    // No resolver: imports load from disk relative to `doc->name`, exactly as stratac does.
    Module* mod = ModuleLoaderLoadSource(&loader, doc->name, doc->text, doc->len);
    ResolveOverloads(mod, &diag, &arena);

    Sb sb;
    SbInit(&sb);
    SbPutc(&sb, '[');
    for (size_t i = 0; i < diag.m_count; i++)
    {
        if (i > 0)
        {
            SbPutc(&sb, ',');
        }
        WriteDiagnostic(&sb, &diag.m_diagnostics[i], doc, &loader);
    }
    SbPutc(&sb, ']');
    SbPutc(&sb, '\0');

    AstDispose((Node*)mod);
    ModuleLoaderDispose(&loader);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    scratch_reset();

    PublishDiagnostics(doc->uri, sb.data);
    SbFree(&sb);

    doc->dirty = false;
}

static bool AnyDirty(void)
{
    for (size_t i = 0; i < g_server.docCount; i++)
    {
        if (g_server.docs[i].dirty)
        {
            return true;
        }
    }

    return false;
}

static void AnalyzeDirtyDocuments(void)
{
    for (size_t i = 0; i < g_server.docCount; i++)
    {
        if (g_server.docs[i].dirty)
        {
            AnalyzeDocument(&g_server.docs[i]);
        }
    }
}

// -- Handlers

static void HandleInitialize(Str id, const JsonValue* params)
{
    // Prefer UTF-8 positions when the client offers them (VS Code only speaks UTF-16).
    g_server.encoding = EncodingUtf16;
    const JsonValue* encodings = JsonGetPath(params, "capabilities.general.positionEncodings");
    if (encodings && encodings->kind == JsonArray)
    {
        for (size_t i = 0; i < encodings->count; i++)
        {
            const JsonValue* e = encodings->items[i];
            if (e->kind == JsonString && StrEqC(e->string, "utf-8"))
            {
                g_server.encoding = EncodingUtf8;
            }
        }
    }

    const JsonValue* dirs = JsonGetPath(params, "initializationOptions.importDirs");
    if (dirs && dirs->kind == JsonArray)
    {
        g_server.importDirs = (char**)calloc(dirs->count ? dirs->count : 1, sizeof(char*));
        for (size_t i = 0; i < dirs->count; i++)
        {
            if (dirs->items[i]->kind == JsonString && dirs->items[i]->string.len > 0)
            {
                g_server.importDirs[g_server.importDirCount++] = DupString(dirs->items[i]->string.data);
            }
        }
    }

    g_server.initialized = true;

    char result[512];
    snprintf(result, sizeof(result),
             "{\"capabilities\":{\"positionEncoding\":\"%s\","
             "\"textDocumentSync\":{\"openClose\":true,\"change\":1}},"
             "\"serverInfo\":{\"name\":\"strata-lsp\",\"version\":\"" STRATA_LSP_VERSION "\"}}",
             g_server.encoding == EncodingUtf8 ? "utf-8" : "utf-16");
    SendResult(id, result);
}

static void HandleDidOpen(const JsonValue* params)
{
    const char* uri = JsonGetString(params, "textDocument.uri");
    const JsonValue* text = JsonGetPath(params, "textDocument.text");
    if (!uri || !text || text->kind != JsonString)
    {
        return;
    }

    SetDocumentText(OpenDocument(uri), text);
}

static void HandleDidChange(const JsonValue* params)
{
    const char* uri = JsonGetString(params, "textDocument.uri");
    const JsonValue* changes = JsonGet(params, "contentChanges");
    Document* doc = uri ? FindDocument(uri) : NULL;
    if (!doc || !changes || changes->kind != JsonArray || changes->count == 0)
    {
        return;
    }

    // Full sync: the last change carries the whole document.
    const JsonValue* text = JsonGet(changes->items[changes->count - 1], "text");
    if (text && text->kind == JsonString)
    {
        SetDocumentText(doc, text);
    }
}

static void HandleDidClose(const JsonValue* params)
{
    const char* uri = JsonGetString(params, "textDocument.uri");
    if (!uri)
    {
        return;
    }

    CloseDocument(uri);
    PublishDiagnostics(uri, "[]");
}

// Returns false once the client has asked the server to exit.
static bool HandleMessage(const char* body, size_t len, int* exitCode)
{
    Arena arena;
    arena_init(&arena, 0);

    const JsonValue* msg = JsonParse(&arena, body, len);
    if (!msg || msg->kind != JsonObject)
    {
        Log("ignoring malformed message");
        arena_free(&arena);
        return true;
    }

    const char* method = JsonGetString(msg, "method");
    const JsonValue* idValue = JsonGet(msg, "id");
    const JsonValue* params = JsonGet(msg, "params");
    Str id = idValue ? idValue->raw : STR_EMPTY;
    bool isRequest = idValue != NULL;
    bool keepRunning = true;

    if (!method)
    {
        // A response to a server->client request; this server sends none.
    }
    else if (strcmp(method, "exit") == 0)
    {
        *exitCode = g_server.shutdownRequested ? 0 : 1;
        keepRunning = false;
    }
    else if (g_server.shutdownRequested)
    {
        if (isRequest)
        {
            SendError(id, RPC_INVALID_REQUEST, "server is shutting down");
        }
    }
    else if (strcmp(method, "initialize") == 0)
    {
        HandleInitialize(id, params);
    }
    else if (!g_server.initialized)
    {
        if (isRequest)
        {
            SendError(id, RPC_SERVER_NOT_INITIALIZED, "server not initialized");
        }
    }
    else if (strcmp(method, "shutdown") == 0)
    {
        g_server.shutdownRequested = true;
        SendResult(id, "null");
    }
    else if (strcmp(method, "textDocument/didOpen") == 0)
    {
        HandleDidOpen(params);
    }
    else if (strcmp(method, "textDocument/didChange") == 0)
    {
        HandleDidChange(params);
    }
    else if (strcmp(method, "textDocument/didClose") == 0)
    {
        HandleDidClose(params);
    }
    else if (isRequest)
    {
        SendError(id, RPC_METHOD_NOT_FOUND, method);
    }
    // Other notifications (initialized, didSave, $/cancelRequest, ...) need no reply.

    arena_free(&arena);
    return keepRunning;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
        {
            printf("strata-lsp " STRATA_LSP_VERSION "\n");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("usage: strata-lsp [--stdio]\n"
                   "Strata language server; speaks LSP over stdin/stdout.\n");
            return 0;
        }
        // `--stdio` (sent by vscode-languageclient) and anything unknown are ignored.
    }

#if defined(STRATA_PLATFORM_WINDOWS)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    int exitCode = 1; // input ended without an `exit` notification

    for (;;)
    {
        // Recompile only once the client has been quiet for a moment, so a burst of
        // keystrokes costs one compile.
        if (AnyDirty() && !WaitForInput(DEBOUNCE_MS))
        {
            AnalyzeDirtyDocuments();
            continue;
        }

        size_t len = 0;
        char* body = ReadMessage(&len);
        if (!body)
        {
            break;
        }

        bool keepRunning = HandleMessage(body, len, &exitCode);
        free(body);

        if (!keepRunning)
        {
            break;
        }
    }

    return exitCode;
}
