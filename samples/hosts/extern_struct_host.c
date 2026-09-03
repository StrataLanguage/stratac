// extern_struct_host.c -- host side of extern_struct.strata.
//
// The host implements the `extern` functions Strata declares. Structs arrive as
// pointers (Strata's in/out/inout lowering) and the layouts match Strata's
// Vec3 { float, float, float } and Point { int, int }.
//   stratac extern_struct.strata -o extern_struct.o
//   clang hosts/extern_struct_host.c extern_struct.o -o extern_struct.exe
//   ./extern_struct.exe
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { float x, y, z; } Vec3;
typedef struct { int x, y; } Point;

float length_sq(const Vec3* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

void scale_into(const Vec3* src, float s, Vec3* dst)
{
    dst->x = src->x * s;
    dst->y = src->y * s;
    dst->z = src->z * s;
}

/* return out-params: void ret + out pointer. */
void MakeVec3(Vec3* out) { out->x = 1; out->y = 2; out->z = 2; }
void NextId(int* out)    { *out = 7; }

/* impl Vec3 methods. */
float Vec3_Length(const Vec3* v)
{
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

float Vec3_StaticSum(float a, float b) { return a + b; }

/* opaque Point: the box cell IS the pointer. */
void GetPoint(Point** out)
{
    Point* p = (Point*)malloc(sizeof(Point));
    if (p)
    {
        p->x = 3;
        p->y = 4;
    }
    *out = p;
}

int  Point_GetX(Point* p)                { return p->x; }
int  Point_GetY(Point* p)                { return p->y; }
void Point_Move(Point* p, int dx, int dy) { p->x += dx; p->y += dy; }

/* The module's ^Point box is freed at scope exit. */
void* strata_alloc(unsigned long n) { return malloc(n); }
void  strata_free(void* p)          { free(p); }
void  strata_panic(const char* m)   { fprintf(stderr, "%s\n", m); }

/* defined in Strata code. */
extern float entry(void);

int main(void)
{
    printf("entry() = %.0f\n", entry());   /* 183 */
    return 0;
}