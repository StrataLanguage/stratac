// engine_api_host.c -- host side of engine_api.strata.
//
// The engine owns the Entity layout. Strata sees `Entity` as an opaque,
// pointer-sized handle, which matches the C `Entity` (a pointer) below.
//   stratac engine_api.strata -o engine_api.o
//   clang hosts/engine_api_host.c engine_api.o -o engine_api.exe
//   ./engine_api.exe
#include <stdio.h>
#include <stdlib.h>

/* The engine's real entity type; Strata never sees inside it. */
typedef struct { int x, y; } Entity;

Entity* world_spawn(int x, int y)
{
    Entity* e = (Entity*)malloc(sizeof(Entity));
    e->x = x;
    e->y = y;

    return e;
}

void world_move(Entity* e, int dx, int dy)
{
    e->x += dx;
    e->y += dy;
}

int world_x(Entity* e) { return e->x; }
int world_y(Entity* e) { return e->y; }

void world_destroy(Entity* e) { free(e); }

/* Strata-provided entry (defined in engine_api.o). */
extern int run(void);

int main(void) {
    printf("run() = %d\n", run());   /* 15 */
    return 0;
}
