// strings_host.c -- host side of strings.strata.
//
//   stratac strings.strata -o strings.o
//   clang hosts/strings_host.c strings.o -o strings.exe
//   ./strings.exe; echo $?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Strata runtime: box/string heap allocation
void* strata_alloc(unsigned long n) { return malloc((size_t)n); }
void strata_free(void* p) { free(p); }

// Strata-provided entry (defined in strings.o).
extern int run(void);

int main(void)
{
    int result = run();
    printf("run() = %d\n", result);
    return 0;
}
