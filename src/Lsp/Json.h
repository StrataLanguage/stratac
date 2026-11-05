#pragma once

// Minimal JSON reader and writer for the language server transport. Parsed values live in an Arena and
// are released with it.

#include "Core/Util.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    JNull,
    JBool,
    JNumber,
    JString,
    JArray,
    JObject,
} JType;

typedef struct JValue JValue;

typedef struct
{
    char* key;
    JValue* value;
} JMember;

struct JValue
{
    JType type;
    bool boolean;
    double number;
    char* str; // JString: NUL-terminated copy
    size_t strLen;
    JValue** items;   // JArray
    JMember* members; // JObject
    size_t count;     // element count for JArray and JObject
};

// Parses one complete JSON document. Returns false on malformed input or trailing content.
bool JsonParse(Arena* arena, const char* text, size_t len, JValue** out);

// Object member lookup; NULL when `obj` is not an object or the key is absent.
const JValue* JsonGet(const JValue* obj, const char* key);

// Writes `s` as a quoted, escaped JSON string.
void JsonPutString(Sb* sb, const char* s, size_t len);

// Writes `v` back as JSON text.
void JsonPutValue(Sb* sb, const JValue* v);
