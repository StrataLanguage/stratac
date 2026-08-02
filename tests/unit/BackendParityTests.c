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
