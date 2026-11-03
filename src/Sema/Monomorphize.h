#pragma once

// Monomorphize: expands generic structs, impls and functions into the concrete declarations that use them.

#include "AST/AST.h"
#include "Core/Diagnostics.h"

// Moves the generic templates out of the module's lists into Module::genericTemplates and rewrites every use
// of them (`number<int>`, `make_number<int>(...)`) into a plain name: the instance's canonical spelling. Each
// instance is a concrete struct, impl or function that sema and codegen treat like any other declaration.
// Reports errors to `diag`; a module without generics is left as it is.
void Monomorphize(Module* mod, DiagnosticEngine* diag, Arena* arena);
