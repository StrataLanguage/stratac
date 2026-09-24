// engine_demo_host.c -- run the SAME Strata script in two modes.
//
//   USE_JIT defined:  load engine_demo.strata at runtime, JIT-compile, call
//                     `chase` through a function pointer (fast iteration).
//   USE_JIT undefined: `chase` was pre-compiled to an object and linked at
//                     build time (ship / production). No runtime compile.
//
// Both modes call the SAME script logic and the SAME engine API. This is the
// dual-mode strategy a game engine can use: develop with JIT, ship with AOT.
//
// Build JIT mode (CMake target 'engine_demo', links libstrata):
//   cmake --build --preset default --target engine_demo
//
// Build AOT mode (run_engine_demo.bat does this):
//   stratac engine_demo.strata -o engine_demo.o
//   clang engine_demo_host.c engine_demo.o -o engine_demo_aot.exe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STRATA_SAMPLE_DIR
#define STRATA_SAMPLE_DIR "."
#endif

void* strata_alloc(size_t sz)
{
    return malloc(sz);
}

void strata_free(void* p)
{
    free(p);
}

/* ---- shared engine API (the "engine" the script calls via extern) -------- */
typedef struct { int x, y; } Entity;

Entity* spawn(int x, int y)
{
    Entity* e = (Entity*)malloc(sizeof(Entity));
    memset(e, 0, sizeof(Entity));

    e->x = x;
    e->y = y;

    return e;
}

void destroy(Entity* e) { free(e); }
int  get_x(Entity* e) { return e->x; }
int  get_y(Entity* e) { return e->y; }
void move(Entity* e, int dx, int dy) { e->x += dx; e->y += dy; }

/* ---- the scene (identical in both modes) -------------------------------- */
/* Like every non-extern Strata function, `chase` takes a hidden leading
   context pointer -- invisible in the .strata source, but a real first
   parameter here. It reads/writes the script's own globals (gStrArray, gStr);
   `ctx` is a per-instance copy of those globals (see __strata_context_create below): spawn one per
   entity so concurrent scripts never clobber each other's state. */
static int run_scene(void* ctx, int (*chase_fn)(void*, Entity*, Entity*, int))
{
    Entity* attacker = spawn(10, 20);
    Entity* target   = spawn(3, 5);

    int result = chase_fn(ctx, attacker, target, 2);
    printf("chase(attacker, target, 2) = %d\n", result);

    destroy(attacker);
    destroy(target);

    return result;
}

#ifdef USE_JIT

//-- JIT

#include "strata/strata.h"

static void* resolve_extern(const char* name) {
    if (strcmp(name, "spawn")   == 0) return (void*)&spawn;
    if (strcmp(name, "destroy") == 0) return (void*)&destroy;
    if (strcmp(name, "get_x")   == 0) return (void*)&get_x;
    if (strcmp(name, "get_y")   == 0) return (void*)&get_y;
    if (strcmp(name, "move")    == 0) return (void*)&move;
    if (strcmp(name, "printf")  == 0) return (void*)&printf;
    return NULL;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : STRATA_SAMPLE_DIR "/engine_demo.strata";
    printf("[JIT] loading %s\n", path);

    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, strata_alloc, strata_free);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileFile(c, path, &err);
    if (!jit) {
        fprintf(stderr, "[JIT] compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        return 1;
    }
    for (size_t i = 0; i < strataJitGetExternSymbolCount(jit); ++i) {
        const char* name = strataJitGetExternSymbolName(jit, i);
        void* fn = resolve_extern(name);
        if (fn) strataJitAddSymbol(jit, name, fn);
    }

    int (*chase_fn)(void*, Entity*, Entity*, int) =
        (int (*)(void*, Entity*, Entity*, int))strataJitGetFunction(jit, "chase");

    if (!chase_fn) { fprintf(stderr, "[JIT] 'chase' not found\n"); return 1; }

    void* (*context_create)(void)  = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void  (*context_destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");

    if (!context_create || !context_destroy) {
        fprintf(stderr, "[JIT] '__strata_context_create'/'__strata_context_destroy' not found\n");
        return 1;
    }

    void* ctx = context_create();
    run_scene(ctx, chase_fn);
    context_destroy(ctx);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}

//--

#else

//-- AOT

extern int chase(void* ctx, Entity* attacker, Entity* target, int step);
extern void* __strata_context_create(void);
extern void __strata_context_destroy(void*);

int main(void) {
    printf("[AOT] script pre-compiled and linked\n");
    void* ctx = __strata_context_create();
    run_scene(ctx, chase);
    __strata_context_destroy(ctx);
    return 0;
}

//--

#endif
