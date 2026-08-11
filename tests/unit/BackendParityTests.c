#include "strata/strata.h"

#include "Codegen/CBackend.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/TccJit.h"
#include "Test.h"
#include "Util.h"

#include <stdio.h>
#include <stdlib.h>

static void CheckParity(const char* source, int expected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(source, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule llvmModule = BuildLlvmModule(mod, &diag, &arena, true);
    LLVMJit llvm;
    LLVMJitInit(&llvm);
    char* llvmError = NULL;
    bool llvmOk = LLVMJitLoad(&llvm, &llvmModule, &llvmError);
    if (!llvmOk)
    {
        printf("  LLVM parity JIT failed: %s\n", llvmError ? llvmError : "(unknown)");
    }
    STRATA_CHECK(llvmOk);

    BuiltCModule cModule = BuildCModule(mod, &diag, &arena, CEmitJIT, STRATA_ARCH_AUTO);
    TccJit tcc;
    TccJitInit(&tcc);
    char* tccError = NULL;
    bool tccOk = TccJitLoad(&tcc, &cModule, &tccError);
    if (!tccOk)
    {
        printf("  TinyCC parity JIT failed: %s\n", tccError ? tccError : "(unknown)");
    }
    STRATA_CHECK(tccOk);

    if (llvmOk && tccOk)
    {
        int (*llvmEntry)(void) = (int (*)(void))(uintptr_t)LLVMJitGetAddress(&llvm, "entry");
        int (*tccEntry)(void) = (int (*)(void))TccJitGetAddress(&tcc, "entry");
        STRATA_CHECK(llvmEntry != NULL);
        STRATA_CHECK(tccEntry != NULL);
        if (llvmEntry && tccEntry)
        {
            STRATA_CHECK_EQ(llvmEntry(), expected);
            STRATA_CHECK_EQ(tccEntry(), expected);
            STRATA_CHECK_EQ(llvmEntry(), tccEntry());
        }
    }

    free(llvmError);
    free(tccError);
    TccJitDestroy(&tcc);
    BuiltCModuleDispose(&cModule);
    LLVMJitDestroy(&llvm);
    BuiltModuleDispose(&llvmModule);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(llvm_and_tcc_cast_to_bool_parity)
{
    /* 2 and 4 are truthy but have a zero low bit: catches bit-truncation. */
    CheckParity("int entry() { return (int)(bool)2 + (int)(bool)4 + (int)(bool)3; }", 3);
}

STRATA_TEST(llvm_and_tcc_box_parity)
{
    CheckParity("struct Cell { int v; };\n"
                "int entry() {\n"
                "  box<Cell> c = Cell { .v = 7 };\n"
                "  c.v = c.v + 35;\n"
                "  return c.v;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_move_parity)
{
    /* Factory return + VarDecl move + assign move, on both backends. */
    CheckParity("struct Cell { int v; };\n"
                "box<Cell> make(int n) { box<Cell> c = Cell { .v = n }; return c; }\n"
                "int entry() {\n"
                "  box<Cell> a = make(10);\n"
                "  box<Cell> b = make(32);\n"
                "  a = b;\n"
                "  return a.v;\n"
                "}\n",
                32);
}

STRATA_TEST(llvm_and_tcc_box_param_owned_parity)
{
    /* An owned box parameter consumes the caller's box (moves it). */
    CheckParity("struct Cell { int v; };\n"
                "int consume(box<Cell> c) { return c.v; }\n"
                "int entry() { box<Cell> a = Cell { .v = 11 }; return consume(a); }\n",
                11);
}

STRATA_TEST(llvm_and_tcc_box_scalar_value_parity)
{
    /* box<int> read as a value (no field to access) in arithmetic and a bare return. */
    CheckParity("int read_bare(box<int> owned) { return owned; }\n"
                "int entry() {\n"
                "  box<int> a = 9;\n"
                "  box<int> b = 5;\n"
                "  int sum = a + b * 2;\n"       /* 9 + 5*2 = 19 */
                "  return sum + read_bare(a);\n" /* a moved into read_bare -> 19 + 9 = 28 */
                "}\n",
                28);
}

STRATA_TEST(llvm_and_tcc_box_value_assigned_into_plain_ref_target_parity)
{
    /* box<Vec3> assigned into a plain `ref Vec3` target derefs on both backends. */
    CheckParity("struct Vec3 { float x; };\n"
                "void mutate(ref Vec3 inBox) { box<Vec3> newBox = Vec3 { .x = 100.0 }; inBox = newBox; }\n"
                "int entry() { Vec3 w = Vec3 { .x = 0.0 }; mutate(w); return (int)w.x; }\n",
                100);
}

STRATA_TEST(llvm_and_tcc_box_same_variable_two_owned_params_parity)
{
    /* The T** ABI means aliasing the same box into two owned params in one
       call must be safe (no double-free) identically on both backends. */
    CheckParity("struct Stage { int fuel; };\n"
                "int ignite(box<Stage> primary, box<Stage> backup) { return primary.fuel + backup.fuel; }\n"
                "int entry() { box<Stage> stage = Stage { .fuel = 50 }; return ignite(stage, stage); }\n",
                100);
}

STRATA_TEST(llvm_and_tcc_box_ref_chain_three_levels_parity)
{
    /* A 3-deep ref-box re-borrow chain must mutate the original identically
       on both backends. */
    CheckParity("struct Stage { int fuel; };\n"
                "void drain_innermost(ref box<Stage> s) { s.fuel = s.fuel - 1; }\n"
                "void relay_b(ref box<Stage> s) { drain_innermost(s); }\n"
                "void relay_a(ref box<Stage> s) { relay_b(s); }\n"
                "int entry() { box<Stage> booster = Stage { .fuel = 100 }; relay_a(booster); return booster.fuel; }\n",
                99);
}

STRATA_TEST(llvm_and_tcc_box_move_and_reassign_in_loop_parity)
{
    /* Move-then-revalidate every loop iteration, checked on both backends. */
    CheckParity("struct Stage { int fuel; };\n"
                "box<Stage> refuel(int amount) { box<Stage> s = Stage { .fuel = amount }; return s; }\n"
                "int burn(box<Stage> s) { return s.fuel; }\n"
                "int entry() {\n"
                "  box<Stage> booster = refuel(5);\n"
                "  int total = 0;\n"
                "  for (int i = 0; i < 50; i++) {\n"
                "    total += burn(booster);\n"
                "    booster = refuel(5);\n"
                "  }\n"
                "  return total;\n"
                "}\n",
                250);
}

STRATA_TEST(llvm_and_tcc_box_dual_owning_field_drop_parity)
{
    /* A struct with two owning box<T> fields must drop both without
       crashing on either backend. */
    CheckParity("struct Sensor { int reading; };\n"
                "struct FlightComputer { box<Sensor> primary; box<Sensor> backup; };\n"
                "int entry() {\n"
                "  box<FlightComputer> fc = FlightComputer {\n"
                "    .primary = Sensor { .reading = 7 },\n"
                "    .backup = Sensor { .reading = 13 }\n"
                "  };\n"
                "  return fc.primary.reading + fc.backup.reading;\n"
                "}\n",
                20);
}

STRATA_TEST(llvm_and_tcc_box_compound_assign_parity)
{
    /* `val -= amt;` through a `ref box<int>` mutates in place on both backends. */
    CheckParity("void sub(ref box<int> val, int amt) { val -= amt; }\n"
                "int entry() { box<int> x = 15; sub(x, 25); return x; }\n",
                -10);
}

STRATA_TEST(llvm_and_tcc_box_passed_to_by_value_scalar_param_parity)
{
    /* A by-value (non-indirect) param - handles hit this same path - must
       receive the dereferenced value, not the box's own heap pointer. */
    CheckParity("int take(int x) { return x; }\n"
                "int entry() { box<int> b = 41; return take(b); }\n",
                41);
}

STRATA_TEST(llvm_and_tcc_box_cast_via_opaque_marker_parity)
{
    /* Allowed since Any is opaque; each cast moves its source. */
    CheckParity("struct Pistol { int ammo; };\n"
                "struct Any;\n"
                "int entry() {\n"
                "  box<Pistol> p = Pistol { .ammo = 77 };\n"
                "  box<Any> a = (box<Any>)p;\n"
                "  box<Pistol> p2 = (box<Pistol>)a;\n"
                "  return p2.ammo;\n"
                "}\n",
                77);
}

STRATA_TEST(llvm_and_tcc_box_param_ref_parity)
{
    /* A ref box parameter borrows the caller's box. */
    CheckParity("struct Cell { int v; };\n"
                "int read(ref box<Cell> c) { return c.v; }\n"
                "int entry() { box<Cell> a = Cell { .v = 5 }; return (read(a) * 10) + a.v; }\n",
                55);
}

STRATA_TEST(llvm_and_tcc_scalar_control_flow_parity)
{
    CheckParity("int entry() { int total = 0; for (int i = 1; i <= 20; i++) { "
                "if (i == 5) { continue; } total += i * i; } return total; }",
                2845);
}

STRATA_TEST(llvm_and_tcc_struct_parity)
{
    CheckParity("struct Pair { int a; int b; }; "
                "int sum(const Pair p) { return p.a + p.b; } "
                "int entry() { Pair p = {.b = 29, .a = 13}; return sum(p); }",
                42);
}

STRATA_TEST(llvm_and_tcc_recursion_parity)
{
    CheckParity("int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } "
                "int entry() { return fib(12); }",
                144);
}

STRATA_TEST(llvm_and_tcc_float_remainder_parity)
{
    CheckParity("int entry() { float x = 7.5; x %= 2.0; return (int)(x * 10.0 + (5.5 % 2.0)); }", 16);
}

STRATA_TEST(llvm_and_tcc_narrow_int_types_parity)
{
    /* Exercises wraparound behavior for byte/sbyte/short/ushort on both backends. */
    CheckParity("int entry() {\n"
                "  byte b = 200;\n"
                "  b = b + 100;\n" /* wraps: 300 % 256 = 44 */
                "  sbyte sb = 100;\n"
                "  sb = sb + 50;\n" /* wraps: 150 - 256 = -106 */
                "  short s = 30000;\n"
                "  s = s + 10000;\n" /* wraps: 40000 - 65536 = -25536 */
                "  ushort us = 60000;\n"
                "  us = us + 10000;\n" /* wraps: 70000 % 65536 = 4464 */
                "  return (int)b + (int)sb + (int)s + (int)us;\n"
                "}\n",
                44 + (-106) + (-25536) + 4464);
}

STRATA_TEST(llvm_and_tcc_array_push_parity)
{
    CheckParity("int entry() {\n"
                "  int[] a = {1, 2, 3};\n"
                "  array_push(a, 4);\n"
                "  array_push(a, 5);\n"
                "  int s = 0;\n"
                "  for (ulong i = 0; i < a.length; i = i + 1) { s = s + a[i]; }\n"
                "  return s;\n"                       /* 1+2+3+4+5 = 15 */
                "}\n",
                15);
}

STRATA_TEST(llvm_and_tcc_array_push_returns_new_length_parity)
{
    CheckParity("int entry() {\n"
                "  int[] a = {7};\n"
                "  ulong n = array_push(a, 8);\n"
                "  return (int)n + (int)a.length;\n"  /* 2 + 2 = 4 */
                "}\n",
                4);
}

STRATA_TEST(llvm_and_tcc_array_pop_parity)
{
    CheckParity("int entry() {\n"
                "  int[] a = {10, 20, 30};\n"
                "  int last = array_pop(a);\n"
                "  return last + (int)a.length + a[1];\n" /* 30 + 2 + 20 = 52 */
                "}\n",
                52);
}

STRATA_TEST(llvm_and_tcc_array_resize_parity)
{
    CheckParity("int entry() {\n"
                "  int[] a = {1, 2, 3, 4, 5};\n"
                "  array_resize(a, 3);\n"
                "  int s1 = a[0] + a[1] + a[2];\n"     /* 6 */
                "  array_resize(a, 5);\n"              /* {1,2,3,0,0} */
                "  int s2 = 0;\n"
                "  for (ulong i = 0; i < a.length; i = i + 1) { s2 = s2 + a[i]; }\n" /* 6 */
                "  return s1 + s2;\n"                   /* 12 */
                "}\n",
                12);
}

STRATA_TEST(llvm_and_tcc_array_push_struct_parity)
{
    CheckParity("struct Pt { int x; };\n"
                "int entry() {\n"
                "  Pt[] a = { Pt{.x = 1}, Pt{.x = 2} };\n"
                "  array_push(a, Pt{.x = 3});\n"
                "  int s = 0;\n"
                "  for (ulong i = 0; i < a.length; i = i + 1) { s = s + a[i].x; }\n"
                "  return s;\n"                        /* 1+2+3 = 6 */
                "}\n",
                6);
}

STRATA_TEST(llvm_and_tcc_array_push_string_move_parity)
{
    /* Pushing a string moves it into the array (source nulled); both strings
       are then freed when the array drops, with no double-free of `s`. */
    CheckParity("int entry() {\n"
                "  string[] a = {\"ab\"};\n"
                "  string s = \"cdef\";\n"
                "  array_push(a, s);\n"
                "  return (int)a.length;\n"            /* 2 */
                "}\n",
                2);
}

STRATA_TEST(llvm_and_tcc_string_global_compiles)
{
    /* A string global used to crash the compiler (a NULL deref in the owning-
       global validation). Both backends must now compile and run it. The
       LLVM backend points the global at a private string constant; the C
       backend strdup's it in module_init and frees it in teardown. */
    CheckParity("string g = \"hello\";\n"
                "int entry() {\n"
                "  return 7;\n"                      /* global is emitted, not read here */
                "}\n",
                7);
}

STRATA_TEST(llvm_and_tcc_box_global_allocates_and_reads_parity)
{
    /* A box<Pt> global is allocated in module_init and freed in teardown;
       entry() can read it and both backends must agree on the value. */
    CheckParity("struct Pt { int x; };\n"
                "box<Pt> g_box = Pt{.x = 42};\n"
                "int entry() {\n"
                "  return g_box.x;\n"                /* 42 */
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_array_global_parity)
{
    /* An int[] global is filled by module_init (LLVM) / module_init (C),
       readable from entry(), and freed in teardown — both backends agree. */
    CheckParity("int[] g_arr = {1, 2, 3};\n"
                "int entry() {\n"
                "  return g_arr[0] + g_arr[1] + g_arr[2] + (int)g_arr.length;\n"  /* 1+2+3+3 = 9 */
                "}\n",
                9);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_array_parity)
{
    /* An owning struct (holds a string) must be boxed; box<S>[] stores
       box<S> elements, each heap-owning its string. Pushing a bare S boxes it,
       and dropping the array recursively frees every S's string in both
       backends without a crash or double-free. */
    CheckParity("struct S { string s; };\n"
                "int entry() {\n"
                "  box<S>[] arr = { S{.s = \"a\"}, S{.s = \"bb\"} };\n"
                "  array_push(arr, S{.s = \"ccc\"});\n"
                "  return (int)arr.length;\n"          /* 3 */
                "}\n",
                3);
}
