#include "strata/strata.h"

#include "Codegen/CodegenBackend.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Test.h"
#include "Util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool Contains(const char* hay, const char* needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* ---- Execution: run a source on the LLVM JIT ---- */

static void CheckParity(const char* source, int expected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(source, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule llvmModule = BuildLlvmModule(mod, &diag, &arena, true, NULL);
    LLVMJit llvm;
    LLVMJitInit(&llvm);
    char* llvmError = NULL;
    bool llvmOk = LLVMJitLoad(&llvm, &llvmModule, &llvmError);
    STRATA_CHECK(llvmOk);

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

/* ---- Host functions for bare extern `...` calls ---- */

static int HostVSum(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        total += va_arg(ap, int);
    }
    va_end(ap);

    return total;
}

static int HostVMean10(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        total += (int)(va_arg(ap, double) * 10.0);
    }
    va_end(ap);

    return total;
}

static void CheckVarargExtern(const char* source, const char* symbol, void* hostFn, int expected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(source, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule llvmModule = BuildLlvmModule(mod, &diag, &arena, true, NULL);
    LLVMJit llvm;
    LLVMJitInit(&llvm);
    char* llvmError = NULL;
    bool llvmOk = LLVMJitLoad(&llvm, &llvmModule, &llvmError);
    STRATA_CHECK(llvmOk);
    if (llvmOk)
    {
        STRATA_CHECK_EQ(LLVMJitAddSymbol(&llvm, symbol, hostFn), 1);
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

/* ---- Parser / sema ---- */

STRATA_TEST(parser_typed_rest_param)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("int sum(int first, int... rest) { return first; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod != NULL);
    if (mod)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
        STRATA_CHECK(fn->isVariadic);
        STRATA_CHECK(!fn->isCVararg);
        STRATA_CHECK_EQ((long)fn->params.count, 2);
        ParamDecl* rest = (ParamDecl*)VecGet(&fn->params, 1);
        STRATA_CHECK(rest->isVarargRest);
        STRATA_CHECK(strcmp(rest->type.name, "int[]") == 0);
    }
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_bare_cvararg_extern)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("extern int printf(string fmt, ...);", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod != NULL);
    if (mod)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
        STRATA_CHECK(fn->isExtern);
        STRATA_CHECK(fn->isVariadic);
        STRATA_CHECK(fn->isCVararg);
    }
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_rest_param_must_be_last)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int f(int... rest, int b) { return 1; }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_bare_cvararg_requires_extern)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int f(int a, ...) { return 1; }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_bare_cvararg_requires_named_param)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("extern int f(...);", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_rest_param_accepts_modifiers)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("int f(ref int... rest) { return 1; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod != NULL);
    if (mod)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
        ParamDecl* rest = (ParamDecl*)VecGet(&fn->params, 0);
        STRATA_CHECK(rest->isVarargRest);
        STRATA_CHECK(rest->mod == ModRef);
    }
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_rest_element_type_mismatch_errors)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int sum(int... rest) { return 0; } int entry() { return sum(1, \"oops\"); }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_variadic_too_few_args_errors)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int sum(int first, int... rest) { return 0; } int entry() { return sum(); }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_struct_in_cvararg_position_errors)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Pt { int x; };\n"
                    "extern int host_vsum(int count, ...);\n"
                    "int entry() { Pt p = {.x = 1}; return host_vsum(1, p); }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Typed rest: execution on both backends ---- */

STRATA_TEST(varargs_typed_rest_int_parity)
{
    CheckParity("int sum(int first, int... rest)\n"
                "{\n"
                "    int total = first;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum(1, 2, 3, 4, 5); }\n",
                15);
}

STRATA_TEST(varargs_typed_rest_zero_args_parity)
{
    CheckParity("int sum(int first, int... rest)\n"
                "{\n"
                "    int total = first;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum(9); }\n",
                9);
}

STRATA_TEST(varargs_typed_rest_float_parity)
{
    CheckParity("float avg(float first, float... rest)\n"
                "{\n"
                "    float total = first;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total / ((float)rest.length + 1.0f);\n"
                "}\n"
                "int entry() { float a = avg(1.0f, 2.0f, 3.0f, 4.0f); return (int)(a * 10.0f); }\n",
                25);
}

STRATA_TEST(varargs_typed_rest_struct_parity)
{
    CheckParity("struct Pt { int x; };\n"
                "int sum_pts(int first, Pt... rest)\n"
                "{\n"
                "    int total = first;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].x; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum_pts(0, Pt{.x = 1}, Pt{.x = 2}, Pt{.x = 3}); }\n",
                6);
}

STRATA_TEST(varargs_typed_rest_box_elements_parity)
{
    /* ^Foo... moves each box source into the collected ^Foo[]. */
    CheckParity("struct Foo { int v; };\n"
                "int sum_box(^Foo... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].v; }\n"
                "    return total;\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "    ^Foo a = Foo{.v = 10};\n"
                "    ^Foo b = Foo{.v = 20};\n"
                "    return sum_box(a, b);\n"   /* 30 */
                "}\n",
                30);
}

STRATA_TEST(varargs_typed_rest_implicit_box_parity)
{
    /* Bare T args to a ^T... rest are boxed inline, like array literals. */
    CheckParity("struct Foo { int v; };\n"
                "int sum_box(^Foo... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].v; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum_box(Foo{.v = 10}, Foo{.v = 20}); }\n",
                30);
}

STRATA_TEST(varargs_typed_rest_box_arg_coerces_to_scalar_parity)
{
    /* ^int args to an int... rest deref to their value (implicit deref),
       NOT a move: the boxes stay usable afterwards. */
    CheckParity("int sum_int(int... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "    ^int x = 5;\n"
                "    ^int y = 7;\n"
                "    int s = sum_int(1, x, y);\n"
                "    return s + x;\n"          /* 13 + 5 = 18 */
                "}\n",
                18);
}

STRATA_TEST(varargs_typed_rest_box_arg_not_moved_for_nonowning_element_parity)
{
    /* ^Pt to a Pt... rest derefs into the array (a copy), not a move:
       the box stays live for later use. */
    CheckParity("struct Pt { int x; };\n"
                "int sum_pts(Pt... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].x; }\n"
                "    return total;\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "    ^Pt p = Pt{.x = 10};\n"
                "    int s = sum_pts(p, Pt{.x = 20});\n"
                "    return s + p.x;\n"        /* 30 + 10 = 40 */
                "}\n",
                40);
}

STRATA_TEST(varargs_typed_rest_box_move_then_reassign_parity)
{
    /* Passing boxes to a ^Foo... rest moves them out (source nulled).
       The moved box can be re-livened by rebinding a fresh box value; no
       double-free or leak on either backend. */
    CheckParity("struct Foo { int v; };\n"
                "int sum_box(^Foo... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].v; }\n"
                "    return total;\n"
                "}\n"
                "^Foo make_box() { ^Foo x = Foo{.v = 5}; return x; }\n"
                "int entry()\n"
                "{\n"
                "    ^Foo a = Foo{.v = 10};\n"
                "    ^Foo b = Foo{.v = 20};\n"
                "    int s = sum_box(a, b);\n"
                "    a = make_box();\n"
                "    return s + a.v;\n"            /* 30 + 5 = 35 */
                "}\n",
                35);
}

STRATA_TEST(varargs_typed_rest_ref_scalar_mutates_parity)
{
    /* ref int... rest borrows the stack array and mutates its elements. */
    CheckParity("int bump(ref int... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1)\n"
                "    {\n"
                "        rest[i] = rest[i] + 1;\n"
                "        total = total + rest[i];\n"
                "    }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return bump(1, 2, 3); }\n",   /* {2,3,4} -> 9 */
                9);
}

STRATA_TEST(varargs_typed_rest_const_scalar_readonly_parity)
{
    /* const int... rest reads the stack array. */
    CheckParity("int sum_const(const int... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum_const(10, 20, 30); }\n",   /* 60 */
                60);
}

STRATA_TEST(varargs_typed_rest_ref_reassign_elements_parity)
{
    /* Reassign every element of a ref rest through direct writes, then read
       them back. */
    CheckParity("int overwrite(ref int... rest)\n"
                "{\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1)\n"
                "    {\n"
                "        rest[i] = (int)i * 10 + 1;\n"
                "    }\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1)\n"
                "    {\n"
                "        total = total + rest[i];\n"
                "    }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return overwrite(1, 2, 3); }\n",   /* {1,11,21} -> 33 */
                33);
}

STRATA_TEST(varargs_typed_rest_ref_reassign_mixed_rw_parity)
{
    /* Interleave reads and writes through a ref rest: overwrite an element,
       then read a later one, then write again. */
    CheckParity("int mix(ref int... rest)\n"
                "{\n"
                "    rest[0] = rest[0] + 100;\n"
                "    int a = rest[1];\n"
                "    rest[2] = rest[0] + rest[1];\n"
                "    return rest[0] + rest[1] + rest[2] + a;\n"
                "}\n"
                "int entry() { return mix(1, 2, 3); }\n",   /* {101,2,103}, a=2 -> 101+2+103+2 = 208 */
                208);
}

STRATA_TEST(varargs_typed_rest_ref_aliases_scalars_parity)
{
    /* A ref int... rest aliases the source variables: element writes mutate
       the caller's variables, and the mutation persists across calls. */
    CheckParity("void set_all(ref int... rest)\n"
                "{\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { rest[i] = 99; }\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "    int x = 1;\n"
                "    int y = 2;\n"
                "    set_all(x, y);\n"
                "    int s = x + y;\n"      /* 198 */
                "    set_all(x, y);\n"
                "    int t = x + y;\n"      /* 198 */
                "    return s + t;\n"       /* 396 */
                "}\n",
                396);
}

STRATA_TEST(varargs_typed_rest_ref_aliases_box_parity)
{
    /* A ^T arg to a ref Foo... rest aliases the boxed value. */
    CheckParity("struct Foo { int v; };\n"
                "void set_first(ref Foo... rest) { rest[0].v = 99; }\n"
                "int entry()\n"
                "{\n"
                "    ^Foo a = Foo{.v = 1};\n"
                "    Foo b = Foo{.v = 2};\n"
                "    set_first(a, b);\n"
                "    return a.v + b.v;\n"   /* 99 + 2 = 101 */
                "}\n",
                101);
}

STRATA_TEST(sema_typed_rest_ref_rebind_rejected)
{
    /* A ref rest borrows the caller's stack array; the binding itself can't
       be reassigned (assign its inner value instead). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int f(ref int... rest) { int[] local = {9, 9}; rest = local; return 0; }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_typed_rest_const_element_write_rejected)
{
    /* Writing through a const rest's elements is rejected. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("int f(const int... rest) { rest[0] = 5; return rest[0]; }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(varargs_typed_rest_ref_box_borrow_parity)
{
    /* ref ^Foo... rest borrows the boxes: the sources stay alive. */
    CheckParity("struct Foo { int v; };\n"
                "int read_boxes(ref ^Foo... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].v; }\n"
                "    return total;\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "    ^Foo a = Foo{.v = 5};\n"
                "    ^Foo b = Foo{.v = 7};\n"
                "    int s = read_boxes(a, b);\n"
                "    return s + a.v + b.v;\n"     /* 12 + 5 + 7 = 24 */
                "}\n",
                24);
}

STRATA_TEST(varargs_typed_rest_struct_stack_buffer_parity)
{
    /* A struct-element rest is stack-collected too. */
    CheckParity("struct Pt { int x; };\n"
                "int sum_pts(Pt... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].x; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum_pts(Pt{.x = 1}, Pt{.x = 2}, Pt{.x = 3}); }\n",   /* 6 */
                6);
}

STRATA_TEST(c_backend_box_array_element_to_by_value_param_parity)
{
    /* A ^T array element (e.g. `^int[] a; a[0]`) passed to a by-value
       T param auto-derefs, like a box ident. */
    CheckParity("int read(int x) { return x; }\n"
                "int entry()\n"
                "{\n"
                "    ^int[] a = { 7 };\n"
                "    return read(a[0]);\n"
                "}\n",
                7);
}

STRATA_TEST(sema_ref_rest_rejects_implicit_boxing)
{
    /* A ref ^T... rest borrows; a bare T can't be boxed inline (it would
       be an owned heap box nobody drops). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo { int v; };\n"
                    "int read_boxes(ref ^Foo... rest) { return 0; }\n"
                    "int entry() { return read_boxes(Foo{.v = 1}); }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_typed_rest_box_used_after_move_error)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo { int v; };\n"
                    "int sum_box(^Foo... rest) { return 0; }\n"
                    "int entry() { ^Foo a = Foo{.v = 1}; int s = sum_box(a); return a.v; }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_typed_rest_ref_box_move_error)
{
    /* A ref ^T can't be moved into a ^T... rest. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo { int v; };\n"
                    "int sum_box(^Foo... rest) { return 0; }\n"
                    "int via_ref(ref ^Foo b) { return sum_box(b); }\n"
                    "int entry() { ^Foo f = Foo{.v = 1}; return via_ref(f); }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(sema_typed_rest_global_box_move_error)
{
    /* A global ^T can't be moved into a ^T... rest. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo { int v; };\n"
                    "int sum_box(^Foo... rest) { return 0; }\n"
                    "^Foo g = Foo{.v = 1};\n"
                    "int entry() { return sum_box(g); }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(varargs_typed_rest_ref_scalar_copies_parity)
{
    /* A plain ref int value is copied into the int... rest array. */
    CheckParity("int sum_int(int... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int via_ref(ref int x, ref int y) { return sum_int(1, x, y); }\n"
                "int entry() { int a = 5; int b = 7; return via_ref(a, b); }\n",   /* 13 */
                13);
}

STRATA_TEST(varargs_typed_rest_ref_struct_copies_parity)
{
    /* A plain ref struct value is copied into the Pt... rest array. */
    CheckParity("struct Pt { int x; };\n"
                "int sum_pts(int first, Pt... rest)\n"
                "{\n"
                "    int total = first;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i].x; }\n"
                "    return total;\n"
                "}\n"
                "int via_ref(ref Pt p, ref Pt q) { return sum_pts(0, p, q); }\n"
                "int entry() { Pt a = {.x = 5}; Pt b = {.x = 7}; return via_ref(a, b); }\n",   /* 12 */
                12);
}

STRATA_TEST(sema_cvararg_box_struct_rejected)
{
    /* A boxed struct can't cross a bare extern `...` by value. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Foo { int v; };\n"
                    "extern int host_vsum(int count, ...);\n"
                    "int entry() { ^Foo f = Foo{.v = 1}; return host_vsum(1, f); }",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(varargs_typed_rest_string_owns_elements_parity)
{
    /* Strings are moved into the collected array; the array is dropped on
       return with no crash or double-free on either backend. */
    CheckParity("int count_strings(string first, string... rest)\n"
                "{\n"
                "    return (int)rest.length + 1;\n"
                "}\n"
                "int entry() { return count_strings(\"a\", \"b\", \"c\", \"d\"); }\n",
                4);
}

STRATA_TEST(varargs_typed_rest_overload_prefers_exact_parity)
{
    CheckParity("int f(int a) { return 1; }\n"
                "int f(int a, int... rest) { return 2; }\n"
                "int entry() { return f(5) + f(5, 6, 7); }\n",
                3);
}

STRATA_TEST(varargs_typed_rest_numeric_widening_parity)
{
    CheckParity("int sum_rest(int... rest)\n"
                "{\n"
                "    int total = 0;\n"
                "    for (ulong i = 0; i < rest.length; i = i + 1) { total = total + rest[i]; }\n"
                "    return total;\n"
                "}\n"
                "int entry() { return sum_rest(1, 2, 3); }\n",
                6);
}

/* ---- Bare extern `...` : execution on both backends ---- */

STRATA_TEST(varargs_extern_cvararg_ints_parity)
{
    CheckVarargExtern("extern int host_vsum(int count, ...);\n"
                      "int entry() { return host_vsum(4, 10, 20, 30, 40); }\n",
                      "host_vsum", (void*)&HostVSum, 100);
}

STRATA_TEST(string_move_chains_read_correctly_via_printf)
{
    /* string reads as its string: printf's return value (chars written)
       proves the content is correct. Covers literal creation, moving a
       string into a local, moves, and reassignment. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry()\n"
                      "{\n"
                      "  string a = \"hello\";\n"
                      "  int n1 = printf(\"box=%s\\n\", a);\n"
                      "  string src = \"world\";\n"
                      "  string b = src;\n"
                      "  int n2 = printf(\"box2=%s\\n\", b);\n"
                      "  string c = a;\n"
                      "  int n3 = printf(\"box3=%s\\n\", c);\n"
                      "  string d = \"test\";\n"
                      "  d = c;\n"
                      "  int n4 = printf(\"box4=%s\\n\", d);\n"
                      "  return n1 + n2 + n3 + n4;\n"
                      "}\n",
                      "printf", (void*)&printf, 43);
}

STRATA_TEST(string_global_reads_correctly_via_printf)
{
    /* A string global initialized from a literal reads correctly. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "string g = \"Hello!\";\n"
                      "int entry() { return printf(\"g=%s\\n\", g); }\n",   /* 9 */
                      "printf", (void*)&printf, 9);
}

STRATA_TEST(string_passes_to_extern_string_param)
{
    /* A string passed to an extern `string` param (by-value const char*)
       reads correctly. */
    CheckVarargExtern("extern int puts(string s);\n"
                      "string g = \"Hello!\";\n"
                      "int entry() { puts(g); string l = \"world\"; puts(l); return 0; }\n",
                      "puts", (void*)&puts, 0);
}

/* ---- string and ^T to extern `...` (C varargs) ---- */

STRATA_TEST(string_to_extern_cvararg_parity)
{
    /* string arg in extern `...` position puns to its char* and printf
       reads it correctly. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  string a = \"alpha\";\n"
                      "  string b = \"beta\";\n"
                      "  return printf(\"%s %s\", a, b);\n"   /* 10 */
                      "}\n",
                      "printf", (void*)&printf, 10);
}

STRATA_TEST(string_from_return_to_extern_parity)
{
    /* string returned from a function then passed to extern: the
       returned value must be readable by printf. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "string make(string s) { string b = s; return b; }\n"
                      "int entry() {\n"
                      "  string g = make(\"gamma\");\n"
                      "  return printf(\"%s\", g);\n"         /* 5 */
                      "}\n",
                      "printf", (void*)&printf, 5);
}

STRATA_TEST(string_multiple_extern_calls_parity)
{
    /* Multiple sequential extern calls with a string verify the value
       survives each call (borrow, not moved) when passed to string param. */
    CheckVarargExtern("extern int puts(string s);\n"
                      "int entry() {\n"
                      "  string b = \"shared\";\n"
                      "  puts(b);\n"
                      "  puts(b);\n"
                      "  puts(b);\n"
                      "  return 0;\n"
                      "}\n",
                      "puts", (void*)&puts, 0);
}

STRATA_TEST(box_int_to_extern_int_param_parity)
{
    /* ^int passed to an extern int param derefs to its value. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  ^int b = 42;\n"
                      "  return printf(\"%d\", b);\n"        /* 2 */
                      "}\n",
                      "printf", (void*)&printf, 2);
}

/* ---- string[] array element to extern ---- */

STRATA_TEST(string_array_element_to_puts_parity)
{
    /* array_push of a string literal into string[] stores the owned fat;
       puts(arr[0]) reads the content correctly. */
    CheckVarargExtern("extern int puts(string s);\n"
                      "int entry() {\n"
                      "  string[] arr;\n"
                      "  array_push(arr, \"hello\");\n"
                      "  puts(arr[0]);\n"
                      "  return 0;\n"
                      "}\n",
                      "puts", (void*)&puts, 0);
}

STRATA_TEST(string_array_element_to_printf_parity)
{
    /* printf("%s", arr[0]) verifies the element's string content is intact
       (not just non-crashing). The return value is the character count. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  string[] arr;\n"
                      "  array_push(arr, \"world\");\n"
                      "  return printf(\"%s\", arr[0]);\n"   /* 5 */
                      "}\n",
                      "printf", (void*)&printf, 5);
}

STRATA_TEST(string_array_multiple_elements_to_printf_parity)
{
    /* Multiple pushes, each must be independently readable. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  string[] arr;\n"
                      "  array_push(arr, \"ab\");\n"
                      "  array_push(arr, \"cd\");\n"
                      "  array_push(arr, \"ef\");\n"
                      "  return printf(\"%s%s%s\", arr[0], arr[1], arr[2]);\n"  /* 6 */
                      "}\n",
                      "printf", (void*)&printf, 6);
}

STRATA_TEST(string_array_literal_element_to_printf_parity)
{
    /* string[] initialized from a literal, then element passed to
       printf — verifies the literal init path owns each string correctly. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  string[] arr = { \"hi\" };\n"
                      "  return printf(\"%s\", arr[0]);\n"   /* 2 */
                      "}\n",
                      "printf", (void*)&printf, 2);
}

STRATA_TEST(string_array_push_var_then_read_parity)
{
    /* Push a string variable (move), then read it back via printf. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "int entry() {\n"
                      "  string[] arr;\n"
                      "  string a = \"moved\";\n"
                      "  array_push(arr, a);\n"
                      "  return printf(\"%s\", arr[0]);\n"   /* 5 */
                      "}\n",
                      "printf", (void*)&printf, 5);
}

STRATA_TEST(string_in_struct_array_to_printf_parity)
{
    /* A ^Holder where Holder has a string field, stored in a
       ^Holder[], then the nested string read via printf. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Holder { string name; };\n"
                      "int entry() {\n"
                      "  ^Holder[] arr;\n"
                      "  array_push(arr, Holder { .name = \"nested\" });\n"
                      "  return printf(\"%s\", arr[0].name);\n"   /* 6 */
                      "}\n",
                      "printf", (void*)&printf, 6);
}

/* ---- string member of a struct passed to functions ---- */

STRATA_TEST(string_member_passed_to_extern_via_printf_parity)
{
    /* A struct with a string field, boxed. The string member is passed to
       printf — verifies the field is strdup'd at init and deref'd correctly
       when passed to an extern string param. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Person { string name; };\n"
                      "int entry() {\n"
                      "  ^Person p = Person { .name = \"Alice\" };\n"
                      "  return printf(\"%s\", p.name);\n"     /* 5 */
                      "}\n",
                      "printf", (void*)&printf, 5);
}

STRATA_TEST(string_member_passed_to_owned_string_param_parity)
{
    /* Pass a string struct field to a function that takes an owned string
       param (moves the string). The field is consumed — the struct must
       still be droppable without double-free. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Person { string name; };\n"
                      "int take(string s) { return printf(\"%s\", s); }\n"
                      "int entry() {\n"
                      "  ^Person p = Person { .name = \"Bob\" };\n"
                      "  return take(p.name);\n"              /* 3 */
                      "}\n",
                      "printf", (void*)&printf, 3);
}

STRATA_TEST(string_member_passed_to_ref_string_param_parity)
{
    /* Pass a string struct field by ref — the string is borrowed, the
       struct still owns it afterward. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Person { string name; };\n"
                      "int read(ref string s) { return printf(\"%s\", s); }\n"
                      "int entry() {\n"
                      "  ^Person p = Person { .name = \"Carol\" };\n"
                      "  int n = read(p.name);\n"
                      "  return n + printf(\" again\", p.name);\n"  /* 5 + 6 = 11 */
                      "}\n",
                      "printf", (void*)&printf, 11);
}

STRATA_TEST(string_member_reassigned_then_read_parity)
{
    /* Reassign the string member of a boxed struct, then read it via
       printf — the old string must be freed and the new one readable. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Person { string name; };\n"
                      "int entry() {\n"
                      "  ^Person p = Person { .name = \"old\" };\n"
                      "  p.name = \"new\";\n"
                      "  return printf(\"%s\", p.name);\n"    /* 3 */
                      "}\n",
                      "printf", (void*)&printf, 3);
}

STRATA_TEST(two_string_members_both_readable_parity)
{
    /* A struct with two string fields — both must be independently strdup'd
       and readable. */
    CheckVarargExtern("extern int printf(string fmt, ...);\n"
                      "struct Pair { string a; string b; };\n"
                      "int entry() {\n"
                      "  ^Pair p = Pair { .a = \"first\", .b = \"second\" };\n"
                      "  return printf(\"%s %s\", p.a, p.b);\n"  /* 12 */
                      "}\n",
                      "printf", (void*)&printf, 12);
}

STRATA_TEST(string_member_passed_to_user_function_parity)
{
    /* A user-defined (non-extern) function that takes a string param.
       The string member is moved into the function, and the function
       drops it. The struct must still be droppable without double-free. */
    CheckParity("struct Item { string label; int qty; };\n"
                "int process(string label, int qty) {\n"
                "  return qty;\n"
                "}\n"
                "int entry() {\n"
                "  ^Item item = Item { .label = \"widget\", .qty = 10 };\n"
                "  return process(item.label, item.qty);\n"  /* 10 */
                "}\n",
                10);
}

STRATA_TEST(varargs_extern_cvararg_float_promotes_to_double_parity)
{
    /* float variadic args must follow C default promotions (widen to double),
       identically on both backends. The host reads doubles via va_arg. */
    CheckVarargExtern("extern int host_vmean10(int count, ...);\n"
                      "int entry() { return host_vmean10(3, 1.5f, 2.5f, 3.0f); }\n",
                      "host_vmean10", (void*)&HostVMean10, 70);
}

STRATA_TEST(varargs_extern_cvararg_zero_variadic_args_parity)
{
    CheckVarargExtern("extern int host_vsum(int count, ...);\n"
                      "int entry() { return host_vsum(0); }\n",
                      "host_vsum", (void*)&HostVSum, 0);
}

STRATA_TEST(varargs_extern_cvararg_box_scalar_derefs_parity)
{
    /* A ^int arg through bare extern `...` derefs to its value (same
       coercion as any by-value box argument), on both backends. */
    CheckVarargExtern("extern int host_vsum(int count, ...);\n"
                      "int entry() { ^int b = 9; return host_vsum(1, b); }\n",
                      "host_vsum", (void*)&HostVSum, 9);
}

/* ---- IR / C emission shapes ---- */

STRATA_TEST(llvm_vararg_extern_declared_variadic)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("extern int printf(string fmt, ...);", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "@printf"));
    STRATA_CHECK(Contains(res.output, ", ...)"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(ast_dump_shows_ellipsis)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("extern int printf(string fmt, ...);\n"
                                  "int sum(int first, int... rest) { return first; }",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    char* dump = DumpAst(mod, &arena);
    STRATA_CHECK(Contains(dump, "..."));
    STRATA_CHECK(Contains(dump, "int... rest"));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
