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

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

/* Compiles `src` without running it and returns its formatted diagnostics ("" when clean). */
static char* Diagnose(const char* src, Arena* arena)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve(src, &diag, arena);

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagHasErrors(&diag) ? DiagFormat(&diag, &sm, 1, arena) : arena_strdup(arena, "");
    DiagnosticEngineFree(&diag);

    return d;
}

STRATA_TEST(literal_char_is_a_byte_constant)
{
    STRATA_CHECK_EQ(RunEntry("int entry() { byte c = 'a'; return c; }\n"), 97);
    STRATA_CHECK_EQ(RunEntry("int entry() { int k = 'A'; return k + ('a' - 'A'); }\n"), 97);
    /* Widened to int first: arithmetic between bytes stays byte (and wraps), as with `(byte)92`. */
    STRATA_CHECK_EQ(RunEntry("int entry() { int b = '\\\\'; return b * 1000 + '\\'' + '\"' + '\\0' + '\\n' + '\\t' + '\\r'; }\n"),
                    92000 + 39 + 34 + 0 + 10 + 9 + 13);
    STRATA_CHECK_EQ(RunEntry("int entry() { string p = \"a\\\\b/c\"; int n = 0;\n"
                             "  for (uint i = 0u; i < p.length; i++) { if (p[i] == '/' || p[i] == '\\\\') { n++; } }\n"
                             "  p[1] = p[1] == '\\\\' ? '/' : p[1]; return n * 100 + p[1]; }\n"),
                    247);
    STRATA_CHECK_EQ(RunEntry("int entry() { byte[4] f = { ' '; 4 }; return f[3]; }\n"), 32);
    STRATA_CHECK_EQ(RunEntry("enum Sep { Slash = '/', Back = '\\\\' };\n"
                             "int entry() { return (int)Sep.Back * 1000 + (int)Sep.Slash; }\n"),
                    92047);
}

STRATA_TEST(literal_char_malformed_is_one_error_each)
{
    Arena arena;
    arena_init(&arena, 0);

    STRATA_CHECK(Contains(Diagnose("int entry() { byte a = ''; return 0; }\n", &arena), "empty character literal"));
    STRATA_CHECK(Contains(Diagnose("int entry() { byte a = 'ab'; return 0; }\n", &arena), "holds one byte"));
    STRATA_CHECK(Contains(Diagnose("int entry() { byte a = '\\q'; return 0; }\n", &arena), "unknown escape '\\q'"));
    STRATA_CHECK(Contains(Diagnose("int entry() { byte a = '\xC3\xA9'; return 0; }\n", &arena), "non-ASCII"));
    STRATA_CHECK(Contains(Diagnose("int entry() { byte a = 'x\n; return 0; }\n", &arena), "unterminated character literal"));
    STRATA_CHECK(!Contains(Diagnose("int entry() { byte a = 'ab'; return 0; }\n", &arena), "expected an expression"));

    arena_free(&arena);
}

STRATA_TEST(handle_is_a_keyword_only_where_a_handle_declaration_starts)
{
    STRATA_CHECK_EQ(RunEntry("handle Base;\nhandle Derived : Base;\n"
                             "struct S { int handle; Base? owner; };\n"
                             "int twice(int handle) { return handle * 2; }\n"
                             "impl S { int Handle(ref S self) { return self.handle; } }\n"
                             "int entry() { ^S s = S { .handle = 4 }; int handle = twice(s.handle); handle = handle + 1;\n"
                             "  return handle + s.Handle(); }\n"),
                    13);

    Arena arena;
    arena_init(&arena, 0);
    STRATA_CHECK(Contains(Diagnose("struct handle { int x; };\nint entry() { return 0; }\n", &arena),
                          "'handle' cannot name a type"));
    STRATA_CHECK(Contains(Diagnose("handle handle;\nint entry() { return 0; }\n", &arena), "'handle' cannot name a type"));
    arena_free(&arena);
}
