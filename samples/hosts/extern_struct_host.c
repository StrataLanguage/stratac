// extern_struct_host.c -- host side of extern_struct.strata.
//
// The host implements the `extern` functions Strata declares. Structs arrive as
// pointers (Strata's in/out/inout lowering), and the layout matches Strata's
// Vec3 { float, float, float }.
//   stratac extern_struct.strata -o extern_struct.o
//   clang hosts/extern_struct_host.c extern_struct.o -o extern_struct.exe
//   ./extern_struct.exe
#include <stdio.h>

typedef struct { float x, y, z; } Vec3;

float length_sq(const Vec3* v) {
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

void scale_into(const Vec3* src, float s, Vec3* dst) {
    dst->x = src->x * s;
    dst->y = src->y * s;
    dst->z = src->z * s;
}

/* Strata-provided entry (defined in extern_struct.o). */
extern float entry(void);

int main(void) {
    printf("entry() = %.0f\n", entry());   /* 125 */
    return 0;
}
