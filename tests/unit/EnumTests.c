/* EnumTests.c — scoped-enum feature tests.
 *
 * An `enum Foo [ : integral ] { A, B = 5, C }` is a strong type alias for its
 * underlying scalar integral type (default `int`), plus scoped constants
 * accessed as `Foo.A`. Covers:
 *   - parse + member value assignment (implicit sequential, explicit, negative)
 *   - underlying type validation and range checks (explicit, sequential, casts)
 *   - strong-alias semantics (no implicit conversion to/from the underlying)
 *   - `impl` on an enum
 *   - end-to-end JIT execution
 */

#include "Util.h"

#if STRATA_TEST_HAS_LLVM
#include "strata/strata.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#endif

#include "Test.h"

#include <stdlib.h>
#include <string.h>

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

static const char* ErrText(DiagnosticEngine* diag, Arena* arena)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < diag->m_count; i++)
    {
        SbPrintf(&sb, "%s; ", diag->m_diagnostics[i].message);
    }

    return SbFinish(&sb, arena);
}

static const EnumDecl* FindEnum(const Module* mod, const char* name)
{
    for (size_t i = 0; i < mod->enums.count; i++)
    {
        const EnumDecl* e = (const EnumDecl*)VecGet((Vec*)&mod->enums, i);

        if (strcmp(e->name, name) == 0)
        {
            return e;
        }
    }

    return NULL;
}

STRATA_TEST(enum_parses_and_assigns_values)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("enum Color : int { Red, Green = 5, Blue };\n"
                                  "enum Dir { North, South, East, West };\n"
                                  "enum Neg : sbyte { A = -2, B, C };\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ(mod->enums.count, 3);

    /* Default underlying is int: sequential 0,1,2,3. */
    const EnumDecl* dir = FindEnum(mod, "Dir");
    STRATA_CHECK(dir != NULL);
    STRATA_CHECK(dir->underlyingType == NULL);
    STRATA_CHECK_EQ(dir->members.count, 4);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&dir->members, 0))->value, 0);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&dir->members, 3))->value, 3);

    /* Explicit + sequential continuation: 0, 5, 6. */
    const EnumDecl* color = FindEnum(mod, "Color");
    STRATA_CHECK(color != NULL);
    STRATA_CHECK(color->underlyingType != NULL);
    STRATA_CHECK(strcmp(color->underlyingType, "int") == 0);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&color->members, 0))->value, 0);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&color->members, 1))->value, 5);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&color->members, 2))->value, 6);

    /* Negative values on a signed underlying: -2, -1, 0. */
    const EnumDecl* neg = FindEnum(mod, "Neg");
    STRATA_CHECK(neg != NULL);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&neg->members, 0))->value, (uint64_t)-2);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&neg->members, 1))->value, (uint64_t)-1);
    STRATA_CHECK_EQ(((EnumMemberDecl*)VecGet((Vec*)&neg->members, 2))->value, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(enum_scoped_member_access)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("enum Color { Red, Green, Blue };\n"
                                  "int entry() { Color c = Color.Blue; return (int)c + (int)Color.Red; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(enum_member_value_is_enum_typed)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    /* The constant's type is the enum: same-enum comparisons are fine. */
    ParseAndResolve("enum Color { Red, Green };\n"
                    "int entry() { Color c = Color.Green; if (c == Color.Green) { return 1; } return 0; }\n",
                    &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* But an enum constant is NOT implicitly convertible to the underlying. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("enum Color { Red, Green };\n"
                    "int entry() { return Color.Red; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(enum_underlying_type_must_be_scalar_integral)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("enum Foo : float { A };\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "must be a scalar integral type"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("struct S { int x; };\n"
                    "enum Foo : S { A };\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "must be a scalar integral type"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(enum_explicit_value_range_checked)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("enum Foo : byte { A = 300 };\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "does not fit in 'byte'"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Sequential continuation overflows too: A=255 forces B=256. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("enum Foo : byte { A = 255, B };\n", &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "does not fit in 'byte'"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);

    /* Negative is rejected for unsigned underlying types. */
    Arena arena3;
    arena_init(&arena3, 0);
    DiagnosticEngine diag3;
    DiagnosticEngineInit(&diag3);
    ParseAndResolve("enum Foo : uint { A = -1 };\n", &diag3, &arena3);
    STRATA_CHECK(DiagHasErrors(&diag3));
    STRATA_CHECK(Contains(ErrText(&diag3, &arena3), "may not be negative"));
    DiagnosticEngineFree(&diag3);
    arena_free(&arena3);

    /* A negative value out of the signed range is also rejected. */
    Arena arena4;
    arena_init(&arena4, 0);
    DiagnosticEngine diag4;
    DiagnosticEngineInit(&diag4);
    ParseAndResolve("enum Foo : sbyte { A = -129 };\n", &diag4, &arena4);
    STRATA_CHECK(DiagHasErrors(&diag4));
    STRATA_CHECK(Contains(ErrText(&diag4, &arena4), "does not fit in 'sbyte'"));
    DiagnosticEngineFree(&diag4);
    arena_free(&arena4);
}

STRATA_TEST(enum_cast_range_checked)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("enum Color : byte { Red, Green };\n"
                    "int entry() { Color c = (Color)300; return 0; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "does not fit"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(enum_duplicate_member_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("enum Foo { A, A };\n", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "duplicate enum member"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(enum_unknown_member_and_assignment_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("enum Foo { A };\n"
                    "int entry() { return (int)Foo.Nope; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "has no member 'Nope'"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("enum Foo { A };\n"
                    "int entry() { Foo.A = 5; return 0; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "cannot assign to enum constant"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

#if STRATA_TEST_HAS_LLVM

typedef struct
{
    const char* name;
    void* fn;
} EnumHostSymbol;

static void CheckEnumImpl(const char* source, const EnumHostSymbol* hosts, size_t hostCount, int expected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(source, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    if (DiagHasErrors(&diag))
    {
        printf("  sema failed: %s\n", ErrText(&diag, &arena));
        DiagnosticEngineFree(&diag);
        arena_free(&arena);
        return;
    }

    BuiltModule llvmModule;
    LLVMJit llvm;
    LLVMJitInit(&llvm);
    char* llvmError = NULL;
    bool llvmOk = false;

    llvmModule = BuildLlvmModule(mod, &diag, &arena, true, NULL);
    llvmOk = LLVMJitLoad(&llvm, &llvmModule, &llvmError);
    if (!llvmOk)
    {
        printf("  LLVM JIT failed: %s\n", llvmError ? llvmError : "(none)");
    }
    STRATA_CHECK(llvmOk);
    if (llvmOk)
    {
        for (size_t i = 0; i < hostCount; i++)
        {
            STRATA_CHECK_EQ(LLVMJitAddSymbol(&llvm, hosts[i].name, hosts[i].fn), 1);
        }
    }

    if (llvmOk)
    {
        int (*llvmEntry)(void) = (int (*)(void))(uintptr_t)LLVMJitGetAddress(&llvm, "entry");
        STRATA_CHECK(llvmEntry != NULL);
        if (llvmEntry)
        {
            STRATA_CHECK_EQ(llvmEntry(), expected);
        }
    }

    free(llvmError);

    LLVMJitDestroy(&llvm);
    BuiltModuleDispose(&llvmModule);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

static int EnumNameLenThunk(int self)
{
    return self;
}

static int EnumCountUpThunk(int self)
{
    return self + 10;
}

STRATA_TEST(enum_jit_end_to_end)
{
    /* Scoped constants of every underlying width + strong-alias equality. */
    CheckEnumImpl("enum Color : int { Red, Green, Blue };\n"
                  "enum Permission : ulong { Read = 1, Write = 2, Execute = 4 };\n"
                  "enum Small : byte { A = 100, B, C };\n"
                  "enum Dir { North, South, East, West };\n"
                  "int entry()\n"
                  "{\n"
                  "  Color c = Color.Blue;\n"
                  "  if (c != (Color)2) { return 90; }\n"
                  "  int sum = (int)Color.Red + (int)Color.Green + (int)Color.Blue;\n"
                  "  if (sum != 3) { return 91; }\n"
                  "  ulong p = (ulong)Permission.Read + (ulong)Permission.Execute;\n"
                  "  if (p != 5) { return 92; }\n"
                  "  if ((int)Small.C != 102) { return 93; }\n"
                  "  if ((int)Dir.East != 2) { return 94; }\n"
                  "  return sum + (int)p + (int)Small.B;\n" /* 3 + 5 + 101 */
                  "}\n",
                  NULL, 0, 109);
}

STRATA_TEST(enum_jit_impl_on_enum)
{
    /* `impl` works on an enum (it is a strong alias): self crosses as the
       underlying scalar by value. */
    EnumHostSymbol hosts[] = { { "Color_NameLen", (void*)&EnumNameLenThunk },
                               { "Color_CountUp", (void*)&EnumCountUpThunk } };
    CheckEnumImpl("enum Color : int { Red, Green, Blue };\n"
                  "impl Color {\n"
                  "    extern int NameLen(Color self);\n"
                  "    extern int CountUp(Color self);\n"
                  "}\n"
                  "int entry()\n"
                  "{\n"
                  "  Color c = Color.Green;\n"
                  "  return c.NameLen() + c.CountUp();\n" /* 1 + 11 */
                  "}\n",
                  hosts, 2, 12);
}

STRATA_TEST(enum_jit_enum_as_array_element)
{
    /* Enums are ordinary strong-alias types: usable as array elements. */
    CheckEnumImpl("enum Dir : int { North, South, East, West };\n"
                  "int entry()\n"
                  "{\n"
                  "  Dir[] dirs = { Dir.North, Dir.South, Dir.East, Dir.West };\n"
                  "  return (int)dirs[1] * 10 + (int)dirs[3];\n" /* 10 + 3 */
                  "}\n",
                  NULL, 0, 13);
}

#endif /* STRATA_TEST_HAS_LLVM */