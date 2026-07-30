// jit_demo_host.c -- load, JIT-compile, and run a Strata file in-process.
//
// This is the embedding demo: a host program uses the Strata C API to compile a
// .strata file to native code at runtime, bind the engine functions it needs,
// and call its entry point through a plain function pointer -- no spawn, no
// link step, no on-disk object.
//
// Build (the CMake `jit_demo` target links libstrata + stages LLVM-C.dll):
//   cmake --build --preset default --target jit_demo
// Run (no args loads samples/jit_demo.strata):
//   build\default\bin\jit_demo.exe
//   build\default\bin\jit_demo.exe path\to\your.strata
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strata/strata.h"

#ifndef STRATA_SAMPLE_DIR
#define STRATA_SAMPLE_DIR "."
#endif

/* ---- the "engine" the script talks to via `extern` ------------------------ */
typedef struct Entity_t { int value; } Entity;

Entity* entity_create(int value)
{
    Entity* e = (Entity*)malloc(sizeof(Entity));
    memset(e, 0, sizeof(Entity));

    e->value = value;

    return e;
}

int entity_get(Entity* e) { return e->value; }
void entity_set(Entity* e, int value) { e->value = value; }
void entity_destroy(Entity* e) { free(e); }

/* Resolve a Strata `extern` name to the host function implementing it. */
static void* resolve_extern(const char* name)
{
    if (strcmp(name, "entity_create")  == 0) return (void*)&entity_create;
    if (strcmp(name, "entity_get")     == 0) return (void*)&entity_get;
    if (strcmp(name, "entity_set")     == 0) return (void*)&entity_set;
    if (strcmp(name, "entity_destroy") == 0) return (void*)&entity_destroy;
    return NULL;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : STRATA_SAMPLE_DIR "/jit_demo.strata";
    printf("== Strata JIT demo ==\n");
    printf("loading %s\n", path);

    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
    if (!jit) {
        fprintf(stderr, "error: JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    /* Discover what the script declared as `extern` and bind each one. */
    size_t nextern = strataJitGetExternSymbolCount(jit);
    printf("script declared %zu extern symbol(s); binding...\n", nextern);
    for (size_t i = 0; i < nextern; ++i) {
        const char* name = strataJitGetExternSymbolName(jit, i);
        void* fn = resolve_extern(name);
        if (fn && strataJitAddSymbol(jit, name, fn)) {
            printf("  bound %-16s -> %p\n", name, fn);
        } else {
            fprintf(stderr, "  no host binding for extern '%s'\n", name);
        }
    }

    /* Resolve the entry to a native function pointer and call it. */
    int (*run)(int) = (int (*)(int))strataJitGetFunction(jit, "run");
    if (!run) {
        fprintf(stderr, "error: entry 'run' not found in the script\n");
        strataJitDestroy(jit);
        strataCompilerDestroy(c);
        return 1;
    }

    int seed = 7;
    int result = run(seed);           /* calls straight into JIT'd native code */
    printf("run(%d) = %d  (expected fibonacci(%d) = 55)\n", seed, result, seed + 3);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}
