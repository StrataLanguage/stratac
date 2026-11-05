#pragma once

// Conversion between `file://` URIs (as editors send them) and filesystem paths.

#include "Core/Util.h"

// Returns the path for a `file://` URI, percent-decoded, or NULL when `uri` is not a file URI.
char* UriToPath(Arena* arena, const char* uri);

// Returns the `file://` URI for an absolute path, percent-encoded.
char* PathToUri(Arena* arena, const char* path);
