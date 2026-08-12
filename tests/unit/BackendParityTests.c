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

STRATA_TEST(llvm_and_tcc_box_string_ownership_parity)
{
    /* box<string> owns its string: create from a literal and from a string
       move, move boxes around, and reassign - all dropped at scope exit with
       no leak or double-free on either backend. */
    CheckParity("int entry()\n"
                "{\n"
                "  box<string> a = \"hello\";\n"
                "  box<string> b = a;\n"
                "  string src = \"world\";\n"
                "  box<string> c = src;\n"
                "  box<string> d = \"test\";\n"
                "  d = b;\n"
                "  return 7;\n"
                "}\n",
                7);
}

STRATA_TEST(llvm_and_tcc_box_string_global_parity)
{
    /* A box<string> global: module_init heap-copies the literal into a box
       slot, module_teardown frees the string and the slot (no crash). */
    CheckParity("box<string> g = \"Hello!\";\n"
                "int entry() { return 7; }\n",
                7);
}

/* ===========================================================================
   Comprehensive box<T> passing-mode matrix.

   For each inner-type category — plain struct, owning struct, string —
   exercise: owned param, ref param, const ref param, factory return,
   reassign, move, global, and as varargs rest elements.
   =========================================================================== */

/* ---- box<plain struct> gaps (most already covered above) ---- */

STRATA_TEST(llvm_and_tcc_box_plain_struct_const_ref_param_parity)
{
    /* const ref box<Plain>: read-only borrow, caller's box still alive. */
    CheckParity("struct Cell { int v; };\n"
                "int peek(const ref box<Cell> c) { return c.v; }\n"
                "int entry() {\n"
                "  box<Cell> a = Cell { .v = 44 };\n"
                "  int r = peek(a);\n"
                "  return r + a.v;\n"             /* 44 + 44 = 88 */
                "}\n",
                88);
}

STRATA_TEST(llvm_and_tcc_box_plain_struct_ref_inner_value_parity)
{
    /* ref Plain (not ref box<Plain>): write through the box into the caller's
       struct slot in-place. */
    CheckParity("struct Cell { int v; };\n"
                "void set(ref Cell target) { target.v = 77; }\n"
                "int entry() {\n"
                "  box<Cell> a = Cell { .v = 0 };\n"
                "  set(a);\n"
                "  return a.v;\n"                 /* 77 */
                "}\n",
                77);
}

STRATA_TEST(llvm_and_tcc_box_plain_struct_const_ref_inner_value_parity)
{
    /* const ref Plain from box<Plain>: read-only deref. */
    CheckParity("struct Cell { int v; };\n"
                "int read(const ref Cell c) { return c.v; }\n"
                "int entry() {\n"
                "  box<Cell> a = Cell { .v = 33 };\n"
                "  int r = read(a);\n"
                "  return r + a.v;\n"             /* 33 + 33 = 66 */
                "}\n",
                66);
}

/* ---- box<owning struct> full matrix ---- */

STRATA_TEST(llvm_and_tcc_box_owning_struct_owned_param_parity)
{
    /* box<owning struct> consumed by owned param: inner box<int> is
       accessible; the whole tree is dropped at function return. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int take(box<Owns> o) { return o.child; }\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 42 };\n"
                "  return take(a);\n"             /* a moved, 42 */
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_ref_param_parity)
{
    /* ref box<owning struct>: borrow and mutate inner field through
       nested box deref. */
    CheckParity("struct Owns { box<int> child; };\n"
                "void bump(ref box<Owns> o) { o.child = o.child + 10; }\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 5 };\n"
                "  bump(a);\n"
                "  bump(a);\n"
                "  return a.child;\n"             /* 5 + 10 + 10 = 25 */
                "}\n",
                25);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_const_ref_param_parity)
{
    /* const ref box<owning struct>: read-only borrow of the whole tree. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int peek(const ref box<Owns> o) { return o.child; }\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 33 };\n"
                "  int r = peek(a);\n"
                "  return r + a.child;\n"         /* 33 + 33 = 66 */
                "}\n",
                66);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_factory_return_parity)
{
    /* Build owning struct in a factory and return it; caller reads inner
       value, everything dropped correctly on both backends. */
    CheckParity("struct Owns { box<int> child; };\n"
                "box<Owns> make(int v) {\n"
                "  box<Owns> o = Owns { .child = v };\n"
                "  return o;\n"
                "}\n"
                "int entry() {\n"
                "  box<Owns> a = make(77);\n"
                "  return a.child;\n"
                "}\n",
                77);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_reassign_parity)
{
    /* Reassigning a box<owning struct> frees the old tree (inner box freed
       first, then the struct, then the outer box). */
    CheckParity("struct Owns { box<int> child; };\n"
                "box<Owns> make(int v) {\n"
                "  box<Owns> o = Owns { .child = v };\n"
                "  return o;\n"
                "}\n"
                "int entry() {\n"
                "  box<Owns> a = make(10);\n"
                "  a = make(33);\n"
                "  return a.child;\n"             /* 33 */
                "}\n",
                33);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_move_vardecl_parity)
{
    /* Move box<owning struct> between locals: inner tree transfers. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 55 };\n"
                "  box<Owns> b = a;\n"
                "  return b.child;\n"             /* 55 */
                "}\n",
                55);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_ref_chain_parity)
{
    /* 3-deep ref chain through owning structs. */
    CheckParity("struct Owns { box<int> child; };\n"
                "void set_inner(ref box<Owns> o, int v) { o.child = v; }\n"
                "void relay(ref box<Owns> o) { set_inner(o, 99); }\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 0 };\n"
                "  relay(a);\n"
                "  return a.child;\n"             /* 99 */
                "}\n",
                99);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_global_parity)
{
    /* box<owning struct> global: module_init, mutate, module_teardown. */
    CheckParity("struct Owns { box<int> child; };\n"
                "box<Owns> g = Owns { .child = 99 };\n"
                "void bump() { g.child = g.child + 1; }\n"
                "int entry() {\n"
                "  bump();\n"
                "  bump();\n"
                "  return g.child;\n"             /* 101 */
                "}\n",
                101);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_two_owning_fields_parity)
{
    /* Struct with two owning fields: both children dropped correctly. */
    CheckParity("struct Leaf { int v; };\n"
                "struct Pair { box<Leaf> a; box<Leaf> b; };\n"
                "int entry() {\n"
                "  box<Pair> p = Pair {\n"
                "    .a = Leaf { .v = 17 },\n"
                "    .b = Leaf { .v = 25 }\n"
                "  };\n"
                "  return p.a.v + p.b.v;\n"      /* 42 */
                "}\n",
                42);
}

/* ---- box<owning struct> as varargs ---- */

STRATA_TEST(llvm_and_tcc_box_owning_struct_to_box_rest_parity)
{
    /* box<owning struct> elements moved into box<Owns>... rest. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int sum_owns(box<Owns>... rest) {\n"
                "  int total = 0;\n"
                "  for (ulong i = 0; i < rest.length; i = i + 1) {\n"
                "    total = total + rest[i].child;\n"
                "  }\n"
                "  return total;\n"
                "}\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 10 };\n"
                "  box<Owns> b = Owns { .child = 20 };\n"
                "  return sum_owns(a, b);\n"      /* 30 */
                "}\n",
                30);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_ref_box_rest_parity)
{
    /* ref box<owning struct>... rest: borrow, sources stay alive. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int sum_ref(ref box<Owns>... rest) {\n"
                "  int total = 0;\n"
                "  for (ulong i = 0; i < rest.length; i = i + 1) {\n"
                "    total = total + rest[i].child;\n"
                "  }\n"
                "  return total;\n"
                "}\n"
                "int entry() {\n"
                "  box<Owns> a = Owns { .child = 10 };\n"
                "  box<Owns> b = Owns { .child = 20 };\n"
                "  int s = sum_ref(a, b);\n"
                "  return s + a.child + b.child;\n" /* 30 + 10 + 20 = 60 */
                "}\n",
                60);
}

/* ---- box<string> passing modes ---- */

STRATA_TEST(llvm_and_tcc_box_string_owned_param_parity)
{
    /* box<string> consumed by owned param: callee drops the string + box
       at return; no crash. */
    CheckParity("int take(box<string> s) { return 42; }\n"
                "int entry() {\n"
                "  box<string> b = \"hello\";\n"
                "  return take(b);\n"             /* b moved */
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_ref_param_parity)
{
    /* ref box<string>: borrow, box still alive after the call. */
    CheckParity("int read(ref box<string> s) { return 7; }\n"
                "int entry() {\n"
                "  box<string> b = \"hello\";\n"
                "  int r = read(b);\n"
                "  return r;\n"                   /* b still alive */
                "}\n",
                7);
}

STRATA_TEST(llvm_and_tcc_box_string_const_ref_param_parity)
{
    /* const ref box<string>: read-only borrow. */
    CheckParity("int peek(const ref box<string> s) { return 8; }\n"
                "int entry() {\n"
                "  box<string> b = \"hello\";\n"
                "  int r = peek(b);\n"
                "  return r;\n"
                "}\n",
                8);
}

STRATA_TEST(llvm_and_tcc_box_string_factory_return_parity)
{
    /* Factory creates box<string> from literal, returns it; caller owns. */
    CheckParity("box<string> make() { box<string> s = \"world\"; return s; }\n"
                "int entry() {\n"
                "  box<string> b = make();\n"
                "  return 42;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_reassign_parity)
{
    /* Reassigning a box<string> frees the old string + box slot. */
    CheckParity("box<string> make() { box<string> s = \"world\"; return s; }\n"
                "int entry() {\n"
                "  box<string> a = \"hello\";\n"
                "  a = make();\n"
                "  return 42;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_move_vardecl_parity)
{
    /* Move box<string> between locals. */
    CheckParity("int entry() {\n"
                "  box<string> a = \"hello\";\n"
                "  box<string> b = a;\n"
                "  return 42;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_in_owning_struct_parity)
{
    /* A struct with a box<string> field is owning; dropping the outer
       box<Holder> frees the inner box<string> and its string. */
    CheckParity("struct Holder { box<string> name; };\n"
                "box<Holder> make(string s) {\n"
                "  box<Holder> h = Holder { .name = s };\n"
                "  return h;\n"
                "}\n"
                "int entry() {\n"
                "  box<Holder> h = make(\"test\");\n"
                "  return 42;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_in_owning_struct_two_fields_parity)
{
    /* Two box<string> fields: both dropped correctly. */
    CheckParity("struct Pair { box<string> a; box<string> b; };\n"
                "int entry() {\n"
                "  box<Pair> p = Pair { .a = \"first\", .b = \"second\" };\n"
                "  return 42;\n"
                "}\n",
                42);
}

STRATA_TEST(llvm_and_tcc_box_string_global_ref_param_parity)
{
    /* A box<string> global borrowed by a ref param. */
    CheckParity("box<string> g = \"global\";\n"
                "int read(ref box<string> s) { return 5; }\n"
                "int entry() {\n"
                "  int r = read(g);\n"
                "  return r;\n"
                "}\n",
                5);
}

/* ---- box<string> as varargs ---- */

STRATA_TEST(llvm_and_tcc_box_string_to_box_string_rest_parity)
{
    /* box<string> to box<string>... rest: move the boxes; rest array dropped
       on return with no leak or double-free. */
    CheckParity("int count(box<string>... rest) { return (int)rest.length; }\n"
                "int entry() {\n"
                "  box<string> a = \"hello\";\n"
                "  box<string> b = \"world\";\n"
                "  return count(a, b);\n"         /* 2 */
                "}\n",
                2);
}

STRATA_TEST(llvm_and_tcc_box_string_ref_box_string_rest_parity)
{
    /* ref box<string>... rest: borrow, sources stay alive. */
    CheckParity("int count(ref box<string>... rest) { return (int)rest.length; }\n"
                "int entry() {\n"
                "  box<string> a = \"hello\";\n"
                "  box<string> b = \"world\";\n"
                "  int n = count(a, b);\n"
                "  return n;\n"                   /* 2 */
                "}\n",
                2);
}

STRATA_TEST(llvm_and_tcc_box_string_loop_soak_parity)
{
    /* Heavy stress: create and drop box<string> every iteration — verifies
       alloc/free balance on both backends at scale. */
    CheckParity("int entry() {\n"
                "  int sum = 0;\n"
                "  for (int i = 0; i < 100; i++) {\n"
                "    box<string> s = \"hello\";\n"
                "    sum = sum + 1;\n"
                "  }\n"
                "  return sum;\n"                 /* 100 */
                "}\n",
                100);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_loop_soak_parity)
{
    /* Heavy stress: create and drop box<owning struct> every iteration. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int entry() {\n"
                "  int sum = 0;\n"
                "  for (int i = 0; i < 100; i++) {\n"
                "    box<Owns> o = Owns { .child = i };\n"
                "    sum = sum + o.child;\n"
                "  }\n"
                "  return sum;\n"                 /* 0+1+...+99 = 4950 */
                "}\n",
                4950);
}

/* ---- box<string>[] array operations ---- */

STRATA_TEST(llvm_and_tcc_box_string_array_push_literal_parity)
{
    /* array_push of a string literal into box<string>[] must box the literal
       (strata_strdup + alloc char* slot), not store the raw char*.  Reading
       the element back and dereferencing must not crash. */
    CheckParity("int entry() {\n"
                "  box<string>[] arr;\n"
                "  array_push(arr, \"hello\");\n"
                "  array_push(arr, \"world\");\n"
                "  return (int)arr.length;\n"      /* 2 */
                "}\n",
                2);
}

STRATA_TEST(llvm_and_tcc_box_string_array_push_move_parity)
{
    /* Pushing a box<string> variable moves it into the array element
       (pointer move, source nulled). */
    CheckParity("int entry() {\n"
                "  box<string>[] arr;\n"
                "  box<string> a = \"first\";\n"
                "  array_push(arr, a);\n"
                "  return (int)arr.length;\n"      /* 1 */
                "}\n",
                1);
}

STRATA_TEST(llvm_and_tcc_box_string_array_literal_init_parity)
{
    /* box<string>[] initialized from a literal: each string literal is
       boxed properly. */
    CheckParity("int entry() {\n"
                "  box<string>[] arr = { \"alpha\", \"beta\", \"gamma\" };\n"
                "  return (int)arr.length;\n"      /* 3 */
                "}\n",
                3);
}

STRATA_TEST(llvm_and_tcc_box_string_array_push_then_pop_parity)
{
    /* Push then pop: the popped box<string> is returned and dropped. */
    CheckParity("int entry() {\n"
                "  box<string>[] arr;\n"
                "  array_push(arr, \"first\");\n"
                "  array_push(arr, \"second\");\n"
                "  array_pop(arr);\n"
                "  return (int)arr.length;\n"      /* 1 */
                "}\n",
                1);
}

STRATA_TEST(llvm_and_tcc_box_string_array_loop_soak_parity)
{
    /* Push a box<string> each iteration, drop the whole array at the end.
       Verifies alloc/free balance at scale on both backends. */
    CheckParity("int entry() {\n"
                "  box<string>[] arr;\n"
                "  for (int i = 0; i < 50; i++) {\n"
                "    array_push(arr, \"item\");\n"
                "  }\n"
                "  return (int)arr.length;\n"      /* 50 */
                "}\n",
                50);
}

STRATA_TEST(llvm_and_tcc_box_struct_array_push_literal_parity)
{
    /* array_push of a bare struct value into box<T>[] must box it inline
       (alloc T slot, store value, store slot pointer). */
    CheckParity("struct Cell { int v; };\n"
                "int entry() {\n"
                "  box<Cell>[] arr;\n"
                "  array_push(arr, Cell { .v = 10 });\n"
                "  array_push(arr, Cell { .v = 20 });\n"
                "  return arr[0].v + arr[1].v;\n"  /* 30 */
                "}\n",
                30);
}

STRATA_TEST(llvm_and_tcc_box_struct_array_push_box_var_parity)
{
    /* Pushing a box<T> variable moves it into the array. */
    CheckParity("struct Cell { int v; };\n"
                "int entry() {\n"
                "  box<Cell>[] arr;\n"
                "  box<Cell> a = Cell { .v = 7 };\n"
                "  array_push(arr, a);\n"
                "  return arr[0].v;\n"             /* 7 */
                "}\n",
                7);
}

STRATA_TEST(llvm_and_tcc_box_string_array_global_push_parity)
{
    /* A box<string>[] global, pushed to from a function, readable later. */
    CheckParity("box<string>[] g_arr;\n"
                "void add_item() { array_push(g_arr, \"global\"); }\n"
                "int entry() {\n"
                "  add_item();\n"
                "  add_item();\n"
                "  return (int)g_arr.length;\n"    /* 2 */
                "}\n",
                2);
}

STRATA_TEST(llvm_and_tcc_box_owning_struct_array_push_parity)
{
    /* Pushing a box<owning struct> moves it; the array's drop frees each
       element's inner box recursively. */
    CheckParity("struct Owns { box<int> child; };\n"
                "int entry() {\n"
                "  box<Owns>[] arr;\n"
                "  array_push(arr, Owns { .child = 10 });\n"
                "  array_push(arr, Owns { .child = 20 });\n"
                "  return arr[0].child + arr[1].child;\n"  /* 30 */
                "}\n",
                30);
}
