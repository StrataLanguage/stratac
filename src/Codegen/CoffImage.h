#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char* name;   /* external symbol name (borrowed; must stay alive until CoffImageLoad returns) */
    void* address;      /* resolved address */
} CoffExtern;

typedef struct CoffImage CoffImage;

/* Loads a COFF x64 object file (as emitted by the LLVM TargetMachine for a
   windows x64 triple) into freshly allocated executable memory: sections are
   laid out honoring their alignment flags, raw data is copied, relocations
   are applied against the provided externs, and every .pdata section's
   RUNTIME_FUNCTION table is registered with the OS dynamic function table
   (RtlAddFunctionTable) so longjmp/exceptions can unwind through the loaded
   code. .pdata entries are kept as RVAs relative to the image base passed to
   RtlAddFunctionTable.

   Returns NULL and sets *errorMessage (malloc-owned, freed by the caller)
   on failure. Windows x64 only; other platforms get a load error. */
CoffImage* CoffImageLoad(const void* objectData, size_t objectSize,
                         const CoffExtern* externs, size_t externCount,
                         char** errorMessage);

/* Final address of a symbol defined in the loaded image, or NULL. */
void* CoffImageGetSymbol(const CoffImage* image, const char* name);

/* Unregisters the function tables and frees the image. */
void CoffImageDestroy(CoffImage* image);
