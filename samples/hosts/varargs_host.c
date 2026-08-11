// varargs_host.c -- host side of varargs.strata.
//
//   stratac varargs.strata -o varargs.o
//   clang hosts/varargs_host.c varargs.o -o varargs.exe
//   ./varargs.exe
#include <stdio.h>
#include <stdlib.h>

// Strata runtime: box/string heap allocation
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg) { fprintf(stderr, "strata panic: %s\n", msg); abort(); }

/* defined in Strata code. */
extern int entry(void);

int main(void)
{
    int result = entry();
    printf("entry() = %d\n", result);
    return 0;
}
