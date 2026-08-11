// arrays_host.c -- host side of arrays.strata.
//
//   stratac arrays.strata -o arrays.o
//   clang hosts/arrays_host.c arrays.o -o arrays.exe
//   ./arrays.exe; echo $?
#include <stdio.h>
#include <stdlib.h>

// Strata runtime: array/box/string heap allocation
void* strata_alloc(size_t n) { return malloc(n); }
void strata_free(void* p) { free(p); }
void strata_panic(const char* msg) { fprintf(stderr, "strata panic: %s\n", msg); abort(); }

// Strata-provided entry (defined in arrays.strata).
extern int entry(void);

int main(void)
{
    int result = entry();
    printf("entry() = %d\n", result);
    return 0;
}
