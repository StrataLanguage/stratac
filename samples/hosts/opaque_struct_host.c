// opaque_struct_host.c -- host side of opaque_struct.strata.
//
// The host owns the layout of `TheType` (Strata only ever sees the type as an
// opaque pointer). The script drives it through `^TheType` boxes plus the
// extern methods declared via `impl TheType`.
//
//   stratac opaque_struct.strata -o opaque_struct.o
//   clang hosts/opaque_struct_host.c opaque_struct.o -o opaque_struct.exe
//   ./opaque_struct.exe
#include <stdio.h>
#include <stdlib.h>

typedef struct { int v; } TheType;

/* return-param out-pointer: writes the box cell the caller owns. */
void GetType(TheType** out)
{
    TheType* p = (TheType*)malloc(sizeof(TheType));
    if (p)
    {
        p->v = 42;
    }
    *out = p;
}

/* impl TheType methods — self crosses as the opaque pointer itself. */
int  TheType_GetValue(TheType* self)        { return self->v; }
void TheType_SetValue(TheType* self, int v) { self->v = v; }
int  TheType_Bump(TheType* self)            { int old = self->v; self->v = old + 1; return old; }

/* The module's ^TheType boxes are dropped at scope exit (strata_free). */
void* strata_alloc(unsigned long n) { return malloc(n); }
void  strata_free(void* p)          { free(p); }
void  strata_panic(const char* m)   { fprintf(stderr, "%s\n", m); }

/* defined in Strata code. */
extern int entry(void);

int main(void)
{
    printf("entry() = %d\n", entry());   /* 87 */
    return 0;
}