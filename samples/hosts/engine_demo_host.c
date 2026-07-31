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
static int run_scene(int (*chase_fn)(Entity*, Entity*, int))
{
    Entity* attacker = spawn(10, 20);
    Entity* target   = spawn(3, 5);
    
    int result = chase_fn(attacker, target, 2);
    printf("chase(attacker, target, 2) = %d\n", result);
    
    destroy(attacker);
    destroy(target);

    return result;
}

/* ======================================================================== */
#ifdef USE_JIT
/* JIT mode: load + compile the script at runtime via the Strata C API.    */
/* ======================================================================== */
#include "strata/strata.h"

static void* resolve_extern(const char* name) {
    if (strcmp(name, "spawn")   == 0) return (void*)&spawn;
    if (strcmp(name, "destroy") == 0) return (void*)&destroy;
    if (strcmp(name, "get_x")   == 0) return (void*)&get_x;
    if (strcmp(name, "get_y")   == 0) return (void*)&get_y;
    if (strcmp(name, "move")    == 0) return (void*)&move;
    return NULL;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : STRATA_SAMPLE_DIR "/engine_demo.strata";
    printf("[JIT] loading %s\n", path);

    StrataCompiler* c = strataCompilerCreate();
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

    int (*chase_fn)(Entity*, Entity*, int) =
        (int (*)(Entity*, Entity*, int))strataJitGetFunction(jit, "chase");

    if (!chase_fn) { fprintf(stderr, "[JIT] 'chase' not found\n"); return 1; }

    run_scene(chase_fn);

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}

#else
/* AOT mode: the script was pre-compiled (stratac) and linked.             */
/* ======================================================================== */
extern int chase(Entity* attacker, Entity* target, int step);

int main(void) {
    printf("[AOT] script pre-compiled and linked\n");
    run_scene(chase);
    return 0;
}
#endif
