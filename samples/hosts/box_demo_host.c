// box_demo_host.c — host side of box_demo.strata.
//
// The host provides `strata_alloc` / `strata_free` (the heap backing for
// box<T>) and implements the `extern` functions Strata declares.  Structs
// arrive as pointers (Strata's by-ref lowering), and the layout must match.
//   stratac box_demo.strata -o box_demo.o
//   clang hosts/box_demo_host.c box_demo.o -o box_demo.exe
//   ./box_demo.exe
#include <stdio.h>
#include <stdlib.h>

void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void  strata_free(void* p)          { free(p); }

typedef struct { float x, y, z; } Vec3;

/* Declared in Strata – the host sees box<T> fields through a regular struct
   pointer (borrow – must not retain the pointer). */
float host_length_sq(const Vec3* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

/* Defined in Strata code. */
extern float entry(void);

int main(void)
{
    printf("entry() = %.0f\n", entry());   /* 27 */
    return 0;
}
