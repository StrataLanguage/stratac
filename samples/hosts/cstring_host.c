// cstring_host.c -- host side of cstring.strata.
//
//   stratac samples/cstring.strata -o cstring.o
//   clang samples/hosts/cstring_host.c cstring.o -o cstring.exe
//   ./cstring.exe
#include <stdio.h>
#include <stdlib.h>

// Strata runtime: the cstring -> string copy allocates.
void* strata_alloc(unsigned long n)
{
    return malloc((size_t)n);
}
void strata_free(void* p)
{
    free(p);
}
void strata_panic(const char* msg)
{
    fprintf(stderr, "strata panic: %s\n", msg);
    abort();
}

// Strata-provided entry (defined in cstring.o).
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
