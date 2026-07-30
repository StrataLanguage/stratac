#define STRATA_TEST_MAIN
#include "strata/Test.h"

static TestRegistration* g_tests = NULL;
static size_t g_testCount = 0;
static size_t g_testCap = 0;
static int g_failures = 0;

void TestRegister(const char* name, TestFn fn)
{
    if (g_testCount >= g_testCap)
    {
        g_testCap = g_testCap ? g_testCap * 2 : 64;
        g_tests = (TestRegistration*)realloc(g_tests, g_testCap * sizeof(TestRegistration));
    }
    g_tests[g_testCount].name = name;
    g_tests[g_testCount].fn = fn;
    g_testCount++;
}

void TestFailAt(const char* expr, const char* file, int line)
{
    ++g_failures;
    printf("  FAIL %s(%d): %s\n", file, line, expr);
}

void TestFailEq(const char* ea, const char* eb, long a, long b, const char* file, int line)
{
    ++g_failures;
    printf("  FAIL %s(%d): %s != %s  (got %ld vs %ld)\n", file, line, ea, eb, a, b);
}

int TestRunAll(void)
{
    int total = 0;
    for (size_t i = 0; i < g_testCount; i++)
    {
        int before = g_failures;
        printf("[ RUN      ] %s\n", g_tests[i].name);
        fflush(stdout);
        g_tests[i].fn();
        fflush(stdout);
        int failed = g_failures - before;
        if (failed == 0)
        {
            printf("[       OK ] %s\n", g_tests[i].name);
        }
        else
        {
            printf("[  FAILED  ] %s (%d assertion(s))\n", g_tests[i].name, failed);
        }
        fflush(stdout);
        ++total;
    }
    printf("== %d test(s), %d failure(s) ==\n", total, g_failures);
    return g_failures;
}
