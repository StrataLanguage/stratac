// extern_math_host.c -- host side of extern_math.strata.
//
// Provides the `extern` functions Strata declares, plus a C main that calls the
// Strata-defined `lucky_number`. Link together with the AOT object:
//   stratac extern_math.strata -o extern_math.o
//   clang hosts/extern_math_host.c extern_math.o -o extern_math.exe
//   ./extern_math.exe
#include <stdio.h>

static int g_rng_state = 0;

int rand_seed(int s)
{
    g_rng_state = s;
    return s;
}

int rand_next(void)
{
    g_rng_state = g_rng_state * 1103515245 + 12345;
    return (g_rng_state >> 16) & 0x7fffffff;
}

int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/* defined in Strata code. */
extern int lucky_number(int seed);

int main(void) {
    int n = lucky_number(42);
    printf("lucky_number(42) = %d\n", n);
    return 0;
}
