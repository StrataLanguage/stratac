#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <stdio.h>
#include <string.h>

/* Runtime checks for literal handling in the lexer/parser (string lengths,
   integer bases, cast-vs-expression parsing), run through the JIT. */

/* Compiles `src`, runs `int entry()`, and returns its result (or -1000 on failure). */
static int RunEntry(const char* src)
{
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "lit", &err);
    strataCompilerDestroy(c);

    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return -1000;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    int r = entry ? entry(NULL) : -1000;
    strataJitDestroy(jit);

    return r;
}

STRATA_TEST(literal_string_with_embedded_nul_keeps_its_length)
{
    STRATA_CHECK_EQ(RunEntry("int entry() { string s = \"ab\\0cd\"; return s.length; }\n"), 5);
    STRATA_CHECK_EQ(RunEntry("int entry() { string s = \"ab\\0cd\"; string t = s; return t.length; }\n"), 5);
}

STRATA_TEST(literal_cstring_with_embedded_nul_stops_at_the_nul)
{
    /* cstring literals are NUL-terminated C strings: an embedded \0 ends them. */
    STRATA_CHECK_EQ(RunEntry("int entry() { cstring c = \"ab\\0cd\"; return c.length; }\n"), 2);
}

STRATA_TEST(literal_string_longer_than_64k)
{
    size_t n = 70000;
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, "int entry() { string s = \"");
    SbPutr(&sb, 'a', n);
    SbPuts(&sb, "\"; return s.length - 69990; }\n");

    Arena arena;
    arena_init(&arena, 0);
    STRATA_CHECK_EQ(RunEntry(SbFinish(&sb, &arena)), 10);
    arena_free(&arena);
}

STRATA_TEST(literal_leading_zero_is_decimal_at_runtime)
{
    STRATA_CHECK_EQ(RunEntry("int entry() { int x = 010; int[010] a = {}; int[09] b = {}; "
                             "return x * 100 + a.length * 10 + b.length - 1000; }\n"),
                    109);
}

STRATA_TEST(literal_paren_variable_minus_is_subtraction)
{
    STRATA_CHECK_EQ(RunEntry("int entry() { int x = 5; int y = (x) - 1; int z = (x) + 2; (x)++; return y * 100 + z * 10 + x; }\n"),
                    476);
    STRATA_CHECK_EQ(RunEntry("int entry() { int x = 5; long y = (long)-x; return (int)y; }\n"), -5);
}
