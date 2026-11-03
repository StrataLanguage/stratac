#include <stdio.h>
#include <stdlib.h>

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
