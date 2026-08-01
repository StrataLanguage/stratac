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
