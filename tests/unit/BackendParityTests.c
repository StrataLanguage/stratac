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
    if (!llvmOk) printf("  LLVM parity JIT failed: %s\n", llvmError ? llvmError : "(unknown)");
    STRATA_CHECK(llvmOk);

    BuiltCModule cModule = BuildCModule(mod, &diag, &arena, true);
    TccJit tcc;
    TccJitInit(&tcc);
    char* tccError = NULL;
    bool tccOk = TccJitLoad(&tcc, &cModule, &tccError);
    if (!tccOk) printf("  TinyCC parity JIT failed: %s\n", tccError ? tccError : "(unknown)");
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
    CheckParity(
        "int entry() { return (int)(bool)2 + (int)(bool)4 + (int)(bool)3; }",
        3);
}

STRATA_TEST(llvm_and_tcc_box_parity)
{
    CheckParity(
        "struct Cell { int v; };\n"
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
    CheckParity(
        "struct Cell { int v; };\n"
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
    CheckParity(
        "struct Cell { int v; };\n"
        "int consume(box<Cell> c) { return c.v; }\n"
        "int entry() { box<Cell> a = Cell { .v = 11 }; return consume(a); }\n",
        11);
}

STRATA_TEST(llvm_and_tcc_box_scalar_value_parity)
{
    /* box<int> read as a value (no field to access) in arithmetic and a bare return. */
    CheckParity(
        "int read_bare(box<int> owned) { return owned; }\n"
        "int entry() {\n"
        "  box<int> a = 9;\n"
        "  box<int> b = 5;\n"
        "  int sum = a + b * 2;\n"        /* 9 + 5*2 = 19 */
        "  return sum + read_bare(a);\n"  /* a moved into read_bare -> 19 + 9 = 28 */
        "}\n",
        28);
}

STRATA_TEST(llvm_and_tcc_box_value_assigned_into_plain_ref_target_parity)
{
    /* box<Vec3> assigned into a plain `ref Vec3` target derefs on both backends. */
    CheckParity(
        "struct Vec3 { float x; };\n"
        "void mutate(ref Vec3 inBox) { box<Vec3> newBox = Vec3 { .x = 100.0 }; inBox = newBox; }\n"
        "int entry() { Vec3 w = Vec3 { .x = 0.0 }; mutate(w); return (int)w.x; }\n",
        100);
}

STRATA_TEST(llvm_and_tcc_box_same_variable_two_owned_params_parity)
{
    /* The T** ABI means aliasing the same box into two owned params in one
       call must be safe (no double-free) identically on both backends. */
    CheckParity(
        "struct Stage { int fuel; };\n"
        "int ignite(box<Stage> primary, box<Stage> backup) { return primary.fuel + backup.fuel; }\n"
        "int entry() { box<Stage> stage = Stage { .fuel = 50 }; return ignite(stage, stage); }\n",
        100);
}

STRATA_TEST(llvm_and_tcc_box_ref_chain_three_levels_parity)
{
    /* A 3-deep ref-box re-borrow chain must mutate the original identically
       on both backends. */
    CheckParity(
        "struct Stage { int fuel; };\n"
        "void drain_innermost(ref box<Stage> s) { s.fuel = s.fuel - 1; }\n"
        "void relay_b(ref box<Stage> s) { drain_innermost(s); }\n"
        "void relay_a(ref box<Stage> s) { relay_b(s); }\n"
        "int entry() { box<Stage> booster = Stage { .fuel = 100 }; relay_a(booster); return booster.fuel; }\n",
        99);
}

STRATA_TEST(llvm_and_tcc_box_move_and_reassign_in_loop_parity)
{
    /* Move-then-revalidate every loop iteration, checked on both backends. */
    CheckParity(
        "struct Stage { int fuel; };\n"
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
    CheckParity(
        "struct Sensor { int reading; };\n"
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
    CheckParity(
        "void sub(ref box<int> val, int amt) { val -= amt; }\n"
        "int entry() { box<int> x = 15; sub(x, 25); return x; }\n",
        -10);
}

STRATA_TEST(llvm_and_tcc_box_passed_to_by_value_scalar_param_parity)
{
    /* A by-value (non-indirect) param - handles hit this same path - must
       receive the dereferenced value, not the box's own heap pointer. */
    CheckParity(
        "int take(int x) { return x; }\n"
        "int entry() { box<int> b = 41; return take(b); }\n",
        41);
}

STRATA_TEST(llvm_and_tcc_box_cast_via_opaque_marker_parity)
{
    /* Allowed since Any is opaque; each cast moves its source. */
    CheckParity(
        "struct Pistol { int ammo; };\n"
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
    CheckParity(
        "struct Cell { int v; };\n"
        "int read(ref box<Cell> c) { return c.v; }\n"
        "int entry() { box<Cell> a = Cell { .v = 5 }; return (read(a) * 10) + a.v; }\n",
        55);
}

STRATA_TEST(llvm_and_tcc_scalar_control_flow_parity)
{
    CheckParity(
        "int entry() { int total = 0; for (int i = 1; i <= 20; i++) { "
        "if (i == 5) { continue; } total += i * i; } return total; }",
        2845);
}

STRATA_TEST(llvm_and_tcc_struct_parity)
{
    CheckParity(
        "struct Pair { int a; int b; }; "
        "int sum(const Pair p) { return p.a + p.b; } "
        "int entry() { Pair p = {.b = 29, .a = 13}; return sum(p); }",
        42);
}

STRATA_TEST(llvm_and_tcc_recursion_parity)
{
    CheckParity(
        "int fib(int n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } "
        "int entry() { return fib(12); }",
        144);
}

STRATA_TEST(llvm_and_tcc_float_remainder_parity)
{
    CheckParity(
        "int entry() { float x = 7.5; x %= 2.0; return (int)(x * 10.0 + (5.5 % 2.0)); }",
        16);
}
