#pragma once

// Language server: document state, request dispatch, and diagnostics. Transport-agnostic apart from
// writing responses to stdout through Transport.h.

#include <stdbool.h>
#include <stddef.h>

typedef struct LspServer LspServer;

LspServer* LspServerCreate(void);
void LspServerDestroy(LspServer* s);

// Adds a directory searched for imports, in the order added (like `stratac -I`).
void LspServerAddImportDir(LspServer* s, const char* dir);

// Handles one framed message body. Returns false once the client has sent `exit`.
bool LspServerHandleMessage(LspServer* s, const char* body, size_t len);

// Process exit status once the server has stopped: 0 after a `shutdown` request, 1 otherwise.
int LspServerExitCode(const LspServer* s);
