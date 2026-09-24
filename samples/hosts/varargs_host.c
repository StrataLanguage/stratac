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
extern int entry(void* ctx);
extern void* __strata_context_create(void);
extern void __strata_context_destroy(void*);

int main(void)
{
    void* ctx = __strata_context_create();
    int result = entry(ctx);
    printf("entry() = %d\n", result);
    __strata_context_destroy(ctx);
    return 0;
}
