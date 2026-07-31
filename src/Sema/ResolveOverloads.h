#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena);
