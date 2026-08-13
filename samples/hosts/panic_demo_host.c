// panic_demo_host.c -- showcase panic-driven stack unwinding in the LLVM JIT.
//
// The script builds owning values (arrays, strings, boxes) across a chain of
// calls and panics at the deepest frame. With StrataProfile.panicUnwind (on
// by default), the panic unwinds the JIT'd stack frame by frame, freeing
// every owning value, until the host boundary: the panic handler runs once
// (after the unwind, with the TLS chain restored) and the entry function
// returns a zeroed value. The host process survives and keeps using the
// module.
//
// Build (the CMake `panic_demo` target links libstrata + stages LLVM-C.dll):
//   cmake --build --preset default --target panic_demo
// Run (no args loads samples/panic_demo.strata):
//   build\default\bin\panic_demo.exe
//   build\default\bin\panic_demo.exe path\to\your.strata
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strata/strata.h"

#ifndef STRATA_SAMPLE_DIR
#define STRATA_SAMPLE_DIR "."
#endif

/* Counting allocator: proves every heap value is freed during the unwind. */
static long s_allocs = 0;
static long s_frees = 0;

static void* count_alloc(unsigned long n)
{
    s_allocs++;
    return malloc((size_t)n);
}

static void count_free(void* p)
{
    s_frees++;
    free(p);
}

static char s_lastPanic[256] = {0};
static int s_panicCount = 0;

/* The handler runs once per panic, AFTER the unwind (owning memory freed,
   unwind chain restored) -- so it is safe to just record and return. */
static void on_panic(const char* msg)
{
    s_panicCount++;
    snprintf(s_lastPanic, sizeof s_lastPanic, "%s", msg ? msg : "(null)");
}

/* extern void host_note(string tag); -- the script reports progress. */
static void host_note(const char* tag)
{
    printf("  [script] %s\n", tag ? tag : "?");
    fflush(stdout);
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : STRATA_SAMPLE_DIR "/panic_demo.strata";

    printf("== Strata panic-unwind demo (LLVM JIT) ==\n");
    printf("loading %s\n", path);

    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        fprintf(stderr, "error: this build has no LLVM JIT (panic unwinding needs it)\n");
        return 1;
    }

    StrataCompiler* c = strataCompilerCreate();
    strataJitSetBackend(c, STRATA_JIT_BACKEND_LLVM); /* unwinding is LLVM-JIT-only */

    /* strataProfileDefault() has panicUnwind = 1; customize via
     * strataJitSetProfile to turn it off and compare behaviors. */

    strataJitSetAllocFreeFunctions(c, (void*)&count_alloc, (void*)&count_free);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
    if (!jit)
    {
        fprintf(stderr, "error: JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    /* Bind the script's externs. */
    size_t nextern = strataJitGetExternSymbolCount(jit);
    for (size_t i = 0; i < nextern; ++i)
    {
        const char* name = strataJitGetExternSymbolName(jit, i);
        if (strcmp(name, "host_note") == 0)
        {
            strataJitAddSymbol(jit, name, (void*)&host_note);
        }
        else
        {
            fprintf(stderr, "warning: no host binding for extern '%s'\n", name);
        }
    }

    /* strataJitGetFunction returns the boundary wrapper: the "last known
     * good location" a panic unwinds to. */
    int (*explode)(void) = (int (*)(void))strataJitGetFunction(jit, "explode");
    int (*safe)(void) = (int (*)(void))strataJitGetFunction(jit, "safe");
    if (!explode || !safe)
    {
        fprintf(stderr, "error: explode/safe entries not found\n");
        strataJitDestroy(jit);
        strataCompilerDestroy(c);
        return 1;
    }

    strataSetPanicHandler(on_panic);

    printf("\n[1] explode(): owning values across 3 frames, panic at the bottom\n");
    s_allocs = s_frees = 0;
    int r1 = explode();
    printf("  -> panic #%d: \"%s\"\n", s_panicCount, s_lastPanic);
    printf("  -> explode() returned %d (zeroed) instead of crashing the host\n", r1);
    printf("  -> allocations: %ld, frees: %ld (%s)\n", s_allocs, s_frees,
           s_allocs == s_frees ? "balanced - unwind freed everything" : "LEAKED");

    printf("\n[2] safe(): the same module keeps working after the panic\n");
    s_allocs = s_frees = 0;
    int before = s_panicCount;
    int r2 = safe();
    printf("  -> safe() = %d (expected 60), panics: %d, allocations: %ld/%ld (%s)\n", r2, s_panicCount - before,
           s_allocs, s_frees, s_allocs == s_frees ? "balanced" : "LEAKED");

    printf("\n[3] explode() again: the unwind chain resets cleanly\n");
    s_allocs = s_frees = 0;
    int r3 = explode();
    printf("  -> panic #%d: \"%s\", returned %d, allocations: %ld/%ld (%s)\n", s_panicCount, s_lastPanic, r3, s_allocs,
           s_frees, s_allocs == s_frees ? "balanced" : "LEAKED");

    strataSetPanicHandler(NULL);

    printf("\nhost process survived: %d panic(s), all memory reclaimed.\n", s_panicCount);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return (r1 == 0 && r2 == 60 && r3 == 0 && s_allocs == s_frees) ? 0 : 1;
}
