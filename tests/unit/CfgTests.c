#include "Sema/Cfg.h"
#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <stdlib.h>
#include <string.h>

static char* DumpFunction(const char* source, const char* name)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(source, &diag, &arena);
    char* out = NULL;

    if (!DiagHasErrors(&diag))
    {
        for (size_t i = 0; i < mod->functions.count; i++)
        {
            const FunctionDecl* f = (const FunctionDecl*)VecGet(&mod->functions, i);

            if (strcmp(f->name, name) == 0)
            {
                Cfg g = CfgBuild(f);
                out = CfgDump(&g);
                CfgFree(&g);
            }
        }
    }

    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
    return out;
}

#define CHECK_DUMP(source, expected)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        char* dump = DumpFunction(source, "F");                                                                        \
        STRATA_CHECK(dump != NULL);                                                                                    \
        if (dump)                                                                                                      \
        {                                                                                                              \
            if (strcmp(dump, expected) != 0)                                                                           \
            {                                                                                                          \
                printf("--- got\n%s--- expected\n%s", dump, expected);                                                 \
            }                                                                                                          \
            STRATA_CHECK(strcmp(dump, expected) == 0);                                                                 \
            free(dump);                                                                                                \
        }                                                                                                              \
    } while (0)

STRATA_TEST(cfg_straight_line)
{
    CHECK_DUMP("void F() { int a = 1; int b = a + 2; }\n",
               "bb0: int declare(a) ident(a) int binary declare(b) scope-exit -> bb1\n"
               "bb1:\n");
}

STRATA_TEST(cfg_if_else_joins)
{
    CHECK_DUMP("void F(bool c) { int a = 0; if (c) { a = 1; } else { a = 2; } a = 3; }\n",
               "bb0: int declare(a) ident(c) cond -> bb2, bb3\n"
               "bb1:\n"
               "bb2: int assign scope-exit -> bb4\n"
               "bb3: int assign scope-exit -> bb4\n"
               "bb4: int assign scope-exit -> bb1\n");
}

STRATA_TEST(cfg_short_circuit_splits)
{
    CHECK_DUMP("void F(bool a, bool b) { bool c = a && b; }\n", "bb0: ident(a) cond -> bb2, bb3\n"
                                                                "bb1:\n"
                                                                "bb2: ident(b) -> bb3\n"
                                                                "bb3: binary declare(c) scope-exit -> bb1\n");
}

STRATA_TEST(cfg_while_break_continue)
{
    /* `break` leaves the loop body and the `if` block (bb5); `continue` leaves the loop body (bb6). */
    CHECK_DUMP("void F(bool c) { while (c) { if (c) { break; } continue; } }\n", "bb0: -> bb2\n"
                                                                                 "bb1:\n"
                                                                                 "bb2: ident(c) cond -> bb3, bb4\n"
                                                                                 "bb3: ident(c) cond -> bb5, bb6\n"
                                                                                 "bb4: scope-exit -> bb1\n"
                                                                                 "bb5: scope-exit scope-exit -> bb4\n"
                                                                                 "bb6: scope-exit -> bb2\n"
                                                                                 "bb7: scope-exit -> bb6\n"
                                                                                 "bb8: scope-exit -> bb2\n");
}

STRATA_TEST(cfg_return_runs_defers_innermost_first)
{
    /* The return value is evaluated first, then G(2) and the inner scope, then G(1) and the outer one. */
    CHECK_DUMP("int G(int x) { return x; }\n"
               "void F() { defer G(1); { defer G(2); return; } }\n",
               "bb0: return int call(G) scope-exit int call(G) scope-exit -> bb1\n"
               "bb1:\n"
               "bb2: int call(G) scope-exit int call(G) scope-exit -> bb1\n");
}
