#pragma once

#include "Core/Util.h"

// Minimal JSON reader/writer for the language server's JSON-RPC traffic.

typedef enum {
    JsonNull,
    JsonBool,
    JsonNumber,
    JsonString,
    JsonArray,
    JsonObject,
} JsonKind;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonKind kind;
    Str raw; // the value's exact source text (used to echo request ids back)

    bool boolean;
    double number;
    Str string; // decoded and NUL-terminated

    // Array elements, or object values paired with `keys`.
    JsonValue** items;
    Str* keys;
    size_t count;
};

// Parses `text` into values allocated from `arena`. NULL on malformed input.
JsonValue* JsonParse(Arena* arena, const char* text, size_t len);

// Member lookup on an object; NULL when `obj` is not an object or has no such key.
const JsonValue* JsonGet(const JsonValue* obj, const char* key);

// Follows a dotted path of object keys, e.g. "textDocument.uri".
const JsonValue* JsonGetPath(const JsonValue* obj, const char* path);

// The string at `path`, or NULL when missing or not a string.
const char* JsonGetString(const JsonValue* obj, const char* path);

// Writes `s` as a quoted, escaped JSON string.
void JsonWriteString(Sb* sb, const char* s, size_t len);
