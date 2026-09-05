/* opt_bench_host.c -- mirrors the engine's ScriptBenchmark workloads so the
 * JIT pipeline can be A/B tested standalone (host malloc, no engine pool).
 *
 * Workloads and methodology (warmup + best-of-5) match
 * hyperion-engine/Source/Engine/Test/Script/ScriptBenchmark.cpp.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "strata/strata.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double NowMs(void)
{
    static double freq = 0.0;
    LARGE_INTEGER f, v;
    if (freq == 0.0) { QueryPerformanceFrequency(&f); freq = (double)f.QuadPart; }
    QueryPerformanceCounter(&v);
    return (double)v.QuadPart / freq;
}
static uint64_t NowTicks(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (uint64_t)v.QuadPart;
}
static double TicksToMs(uint64_t ticks)
{
    static double freq = 0.0;
    LARGE_INTEGER f;
    if (freq == 0.0) { QueryPerformanceFrequency(&f); freq = (double)f.QuadPart; }
    return (double)ticks / freq * 1000.0;
}
#else
#include <time.h>
static uint64_t NowTicks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec / 1000;
}
static double TicksToMs(uint64_t ticks) { return (double)ticks / 1000.0; }
#endif

#define REPS 5

static const char* kSource =
    "long int_loop(int n)\n"
    "{\n"
    "    long total = 0;\n"
    "    for (int i = 0; i < n; i++) { total = total + i; }\n"
    "    return total;\n"
    "}\n"
    "\n"
    "long fib(int n)\n"
    "{\n"
    "    long a = 0;\n"
    "    long b = 1;\n"
    "    for (int i = 0; i < n; i++) { long next = a + b; a = b; b = next; }\n"
    "    return a;\n"
    "}\n"
    "\n"
    "long mandelbrot(int res)\n"
    "{\n"
    "    long count = 0;\n"
    "    for (int y = 0; y < res; y++)\n"
    "    {\n"
    "        double ci = -1.0 + (2.0 * (double)y) / (double)res;\n"
    "        for (int x = 0; x < res; x++)\n"
    "        {\n"
    "            double cr = -2.0 + (2.5 * (double)x) / (double)res;\n"
    "            double zr = 0.0;\n"
    "            double zi = 0.0;\n"
    "            int iter = 0;\n"
    "            while (iter < 64)\n"
    "            {\n"
    "                double zr2 = zr * zr;\n"
    "                double zi2 = zi * zi;\n"
    "                if (zr2 + zi2 > 4.0) { break; }\n"
    "                zi = 2.0 * zr * zi + ci;\n"
    "                zr = zr2 - zi2 + cr;\n"
    "                iter++;\n"
    "            }\n"
    "            count = count + iter;\n"
    "        }\n"
    "    }\n"
    "    return count;\n"
    "}\n"
    "\n"
    "int add_one(int x)\n"
    "{\n"
    "    return x + 1;\n"
    "}\n"
    "\n"
    "long call_overhead(int n)\n"
    "{\n"
    "    int total = 0;\n"
    "    for (int i = 0; i < n; i++) { total = add_one(total); }\n"
    "    return total;\n"
    "}\n"
    "\n"
    "long array_access(int n)\n"
    "{\n"
    "    int[] a = {};\n"
    "    array_resize(a, n);\n"
    "    for (int i = 0; i < n; i++) { a[i] = i; }\n"
    "    long total = 0;\n"
    "    for (ulong i = 0; i < a.length; i++) { total = total + a[i]; }\n"
    "    return total;\n"
    "}\n"
    "\n"
    "long string_alloc(int n)\n"
    "{\n"
    "    string[] arr = {};\n"
    "    for (int i = 0; i < n; i++) { array_push(arr, \"hello world\"); }\n"
    "    long total = 0;\n"
    "    for (ulong i = 0; i < arr.length; i++) { total = total + (long)arr[i].length; }\n"
    "    return total;\n"
    "}\n";

typedef int64_t (*IntFn)(int32_t);

typedef struct
{
    const char* name;
    int32_t arg;
    int64_t expected;
    const char* fnName;
} Workload;

static const Workload kWorkloads[] = {
    { "int_loop",      1000000, 499999500000LL, "int_loop" },
    { "fib",                46, 1836311903LL,   "fib" },
    { "mandelbrot",        128, 402802LL,       "mandelbrot" },
    { "call_overhead",  100000, 100000LL,       "call_overhead" },
    { "array_access",   100000, 4999950000LL,   "array_access" },
    { "string_alloc",    10000, 110000LL,       "string_alloc" },
};

/* Baseline reference timings for C# / Python (same machine, same method),
   printed as context columns next to the JIT result. */
static double BestOf(IntFn fn, int32_t arg, int64_t* outResult)
{
    *outResult = fn(arg); /* warmup */

    double bestMs = 1e300;

    for (int i = 0; i < REPS; i++)
    {
        uint64_t t0 = NowTicks();
        *outResult = fn(arg);
        double ms = TicksToMs(NowTicks() - t0);

        if (ms < bestMs) { bestMs = ms; }
    }

    return bestMs;
}

int main(void)
{
    printf("== strata JIT opt bench (best of %d) ==\n", REPS);

    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, kSource, "opt_bench", &err);

    if (!jit)
    {
        fprintf(stderr, "JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return 1;
    }

    printf("%-15s %12s %10s  %s\n", "workload", "Strata(ms)", "result", "ok");

    for (size_t i = 0; i < sizeof(kWorkloads) / sizeof(kWorkloads[0]); i++)
    {
        Workload w = kWorkloads[i];

        IntFn fn = (IntFn)(uintptr_t)strataJitGetFunction(jit, w.fnName);

        if (!fn)
        {
            printf("%-15s %12s %10s  MISSING\n", w.name, "-", "-");
            continue;
        }

        int64_t result = 0;
        double ms = BestOf(fn, w.arg, &result);

        printf("%-15s %12.3f %10lld  %s\n", w.name, ms, (long long)result,
               result == w.expected ? "PASS" : "FAIL");
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
    return 0;
}
