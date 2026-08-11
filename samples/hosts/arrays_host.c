// arrays_host.c -- host side of arrays.strata.
//
//   stratac arrays.strata -o arrays.o
//   clang hosts/arrays_host.c arrays.o -o arrays.exe
//   ./arrays.exe; echo $?
#include <stdio.h>
#include <stdlib.h>

// Strata runtime: array/box/string heap allocation
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }

// Strata-provided entry (defined in arrays.o).
extern int entry(void);

int main(void)
{
    int result = entry();
    printf("entry() = %d\n", result);
    return 0;
}
