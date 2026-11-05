#pragma once

// Content-Length framed messages over a byte stream, as the language server protocol uses on stdio.

#include <stddef.h>
#include <stdio.h>

// Reads one framed message body (malloc'd, NUL-terminated; length in *outLen). Returns NULL at end of input
// or on a malformed frame. The caller frees the result.
char* LspReadMessage(FILE* in, size_t* outLen);

// Writes one framed message and flushes it.
void LspWriteMessage(FILE* out, const char* body, size_t len);
