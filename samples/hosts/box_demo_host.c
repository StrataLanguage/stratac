// box_demo_host.c — host side of box_demo.strata.
//
// Uses the public JIT API (strataJitCompileFile) to load and execute the
// sample at runtime.  The engine / host binary are compiled together via
// CMake (see CMakeLists.txt).
//
// The host provides `strata_alloc` / `strata_free` as JIT symbols so that
// generated box code can allocate on the host heap.
#include "strata/strata.h"

#include <stdio.h>

typedef struct { float x, y, z; } Vec3;

static float host_length_sq(const Vec3* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

typedef struct { const char* name; } BaseEntity;
typedef struct { BaseEntity base; } Entity;

typedef Entity* HEntity;

Entity g_entity;

static void get_entity(HEntity* outEntityHandle)
{
    g_entity.base.name = "TestEntity";

    *outEntityHandle = &g_entity;
}

static void print_entity_name(HEntity inEntity)
{
    puts(inEntity->base.name);
}

static void handle_panic(const char* str)
{
    printf("STRATA PANIC! %s\n", str);
}

int main(void)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;

    StrataProfile profile;
    profile.boundsCheck = (unsigned)1;
    profile.nullExternCall = (unsigned)1;

    strataJitSetBackend(c, STRATA_JIT_BACKEND_LLVM);
    strataJitSetProfile(c, &profile);

    strataSetPanicHandler(handle_panic);

    char path[512];
    snprintf(path, sizeof path, "%s/box_demo.strata", STRATA_SAMPLE_DIR);
    StrataJit* jit = strataJitCompileFile(c, path, &err);

    if (!jit)
    {
        fprintf(stderr, "JIT compile failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    /* Bind host externs so generated code can call back (strata_alloc / strata_free
       are already wired by the runtime). */
    strataJitAddSymbol(jit, "host_length_sq", (void*)&host_length_sq);
    strataJitAddSymbol(jit, "get_entity", (void*)&get_entity);
    strataJitAddSymbol(jit, "print_entity_name", (void*)&print_entity_name);
    strataJitAddSymbol(jit, "printf", (void*)&printf);

    float (*entry)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");

    if (entry)
    {
        printf("entry() = %.0f\n", entry());   /* 27 */
    }
    else
    {
        fprintf(stderr, "could not resolve 'entry'");
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}
