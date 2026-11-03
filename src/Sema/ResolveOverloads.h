#pragma once
// Entry point of overload resolution, run once per module.

#include "AST/AST.h"
#include "Core/Diagnostics.h"

void ResolveOverloads(Module* mod, DiagnosticEngine* diag, Arena* arena);
