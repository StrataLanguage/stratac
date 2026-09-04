/* ExternTests.c — extern (C) functions crossing the host boundary.
 *
 * Covers the full passing matrix for the owning types that most commonly
 * cross into C code: string (plain / const / ref / const ref), arrays
 * (int[] / ref int[] / string[]), and boxes (^T, ^string, ^T[]),
 * each as parameters AND return values. All scenarios run on the LLVM JIT.
 *
 * ABI notes locked in here:
 *   - extern `string` params cross by value as `const char*`, even when
 *     spelled `ref string` / `const ref string` (the host reads content).
 *   - extern `string` returns are OWNED: the caller frees the returned
 *     buffer, so hosts must hand back malloc'd memory.
 *   - extern array params cross as a pointer to the {data, len} fat struct;
 *     a `ref int[]` host can rewrite the whole array (out-param).
 *   - extern array returns are OWNED: the caller frees `.data`, so hosts
 *     must malloc the buffer. Returning the 16-byte struct across the host
 *     boundary is ABI-host-dependent (sret vs register return), so this is
 *     not exercised here.
 *   - extern ^T / T? params cross as ONE pointer by value (the box cell;
 *     NULL = empty for T?). ^T returns are OWNED.
 *   - extern owned params never move the caller's value (borrowed).
 */

#include "strata/strata.h"

#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Test.h"
#include "Util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Host ABI types ---- */

/* Layout-identical to the compiler's `strata__arr` (and the LLVM array
   struct type): { data pointer, u64 length }. */
typedef struct
{
    void* data;
    unsigned long long len;
} HostArr;

/* Layout-identical to a strata `struct Foo { int v; };`. */
typedef struct
{
    int v;
} HostFoo;

static char* HostDup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* out = (char*)malloc(n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

/* ---- String hosts ---- */

static int HostStrLen(const char* s)
{
    return (int)strlen(s);
}

static int HostStrLenConst(const char* s)
{
    return (int)strlen(s);
}

static int HostStrLenRef(const char* s)
{
    return (int)strlen(s);
}

static int HostStrLenConstRef(const char* s)
{
    return (int)strlen(s);
}

/* Mutates the caller's string buffer in place; returns the new first char. */
static int HostStrShout(char* s)
{
    for (char* p = s; *p; p++)
    {
        if (*p >= 'a' && *p <= 'z')
        {
            *p -= 32;
        }
    }
    return (unsigned char)s[0];
}

/* Returns an OWNED buffer the caller will free. */
static const char* HostStrMake(void)
{
    return HostDup("fromhost");
}

/* Returns an OWNED buffer; params are by-value const char*. */
static const char* HostStrConcat(const char* a, const char* b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    char* out = (char*)malloc(na + nb + 1);
    if (out)
    {
        memcpy(out, a, na);
        memcpy(out + na, b, nb + 1);
    }
    return out;
}

/* ---- Array hosts ---- */

static int HostArrSum(const int* a, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += a[i];
    }
    return total;
}

static void HostArrBump(HostArr* a)
{
    int* p = (int*)a->data;
    for (unsigned long long i = 0; i < a->len; i++)
    {
        p[i] += 1;
    }
}

/* Out-param: replaces the caller's array with {0, 10, 20, ...}. */
static void HostArrFill(HostArr* a, int n)
{
    int* p = (int*)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++)
    {
        p[i] = i * 10;
    }
    a->data = p;
    a->len = (unsigned long long)n;
}

/* Returns an OWNED array buffer the caller will free. */
static HostArr HostArrMake(int n)
{
    HostArr r = {0};
    int* p = (int*)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++)
    {
        p[i] = i * 10;
    }
    r.data = p;
    r.len = (unsigned long long)n;
    return r;
}

static int HostStrArrCount(const HostArr* a)
{
    return (int)a->len;
}

/* ---- Array-decay-to-^T hosts ----
   An extern `^T` param accepts a T[] / T[N] argument by decaying it to a
   pointer to its first element, so the host sees a plain `T*` (the same ABI
   as C's `T arr[N]` / `T*` params). */
static int HostFloatSum(const float* a, int n)
{
    float s = 0;
    for (int i = 0; i < n; i++)
    {
        s += a[i];
    }
    return (int)s;
}

static int HostDoubleSum(const double* a, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++)
    {
        s += a[i];
    }
    return (int)s;
}

static int HostIntSum(const int* a, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
    {
        s += a[i];
    }
    return s;
}

static void HostIntBump(int* a, int n)
{
    for (int i = 0; i < n; i++)
    {
        a[i] += 1;
    }
}

static void HostIntFill(int* a, int n)
{
    for (int i = 0; i < n; i++)
    {
        a[i] = i * 2;
    }
}

static int HostByteSum(const unsigned char* a, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
    {
        s += a[i];
    }
    return s;
}

static int HostFloatIntMix(const float* f, int fn, const int* i, int in)
{
    return (int)f[0] + i[0] + fn + in;
}

/* ---- Box hosts ----
   Extern ^T / T? params cross as the pointer ITSELF (the box cell), so a
   host reads and mutates the boxed value through one deref. NULL is only
   possible for `T?` params. */

static int HostBoxRead(HostFoo* b)
{
    return b->v;
}

static void HostBoxSet(HostFoo* b)
{
    b->v = 99;
}

static int HostBoxReadRef(HostFoo* b)
{
    return b->v;
}

/* Returns an OWNED box slot the caller will free. */
static HostFoo* HostBoxMake(int v)
{
    HostFoo* f = (HostFoo*)malloc(sizeof(HostFoo));
    if (f)
    {
        f->v = v;
    }
    return f;
}

static int HostStrBoxLen(char** b)
{
    /* ^string crosses as the cell pointer: one deref reaches the chars. */
    return (int)strlen(*b);
}

static int HostBoxArrFirst(const HostArr* a)
{
    HostFoo** elems = (HostFoo**)a->data;
    return elems[0]->v;
}

/* ---- Harness: run a source on the LLVM JIT with host symbols bound ---- */

typedef struct
{
    const char* name;
    void* fn;
} HostSymbol;

static void CheckExternImpl(const char* source, const HostSymbol* hosts, size_t hostCount, int expected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(source, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    if (DiagHasErrors(&diag))
    {
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

static void CheckLlvmOnlyExtern(const char* source, const HostSymbol* hosts, size_t hostCount, int expected)
{
    CheckExternImpl(source, hosts, hostCount, expected);
}

static void CheckExtern(const char* source, const HostSymbol* hosts, size_t hostCount, int expected)
{
    CheckExternImpl(source, hosts, hostCount, expected);
}

/* ================= String params ================= */

STRATA_TEST(extern_string_param_by_value)
{
    HostSymbol hosts[] = { { "host_str_len", (void*)&HostStrLen } };
    CheckExtern("extern int host_str_len(string s);\n"
                "int entry()\n"
                "{\n"
                "  string s = \"hello\";\n"
                "  int n = host_str_len(s);\n"       /* 5; s is borrowed, not moved */
                "  s = \"world\";\n"
                "  return n + host_str_len(\"hi\") + host_str_len(s);\n" /* 5 + 2 + 5 = 12 */
                "}\n",
                hosts, 1, 12);
}

STRATA_TEST(extern_const_string_param_by_value)
{
    HostSymbol hosts[] = { { "host_str_len_const", (void*)&HostStrLenConst } };
    CheckExtern("extern int host_str_len_const(const string s);\n"
                "int entry()\n"
                "{\n"
                "  string v = \"abcde\";\n"
                "  return host_str_len_const(v) + host_str_len_const(\"fg\");\n" /* 5 + 2 = 7 */
                "}\n",
                hosts, 1, 7);
}

STRATA_TEST(extern_ref_string_param_reads_content)
{
    /* `ref string` (and `const ref string`) still cross by value as a
       const char* at the extern ABI; the host reads the string content. */
    HostSymbol hosts[] = { { "host_str_len_ref", (void*)&HostStrLenRef } };
    CheckExtern("extern int host_str_len_ref(ref string s);\n"
                "int entry()\n"
                "{\n"
                "  string s = \"xyz\";\n"
                "  return host_str_len_ref(s);\n"    /* 3 */
                "}\n",
                hosts, 1, 3);
}

STRATA_TEST(extern_const_ref_string_param_reads_content)
{
    HostSymbol hosts[] = { { "host_str_len_cref", (void*)&HostStrLenConstRef } };
    CheckExtern("extern int host_str_len_cref(const ref string s);\n"
                "int entry()\n"
                "{\n"
                "  string s = \"qwerty\";\n"
                "  return host_str_len_cref(s);\n"   /* 6 */
                "}\n",
                hosts, 1, 6);
}

STRATA_TEST(extern_ref_string_host_mutates_in_place)
{
    /* The by-value char* still points at the caller's owned buffer, so a
       host that casts away const can mutate it in place. */
    HostSymbol hosts[] = { { "host_str_shout", (void*)&HostStrShout } };
    CheckExtern("extern int host_str_shout(ref string s);\n"
                "int entry()\n"
                "{\n"
                "  string s = \"abc\";\n"
                "  return host_str_shout(s);\n"      /* mutates to \"ABC\", returns 'A' = 65 */
                "}\n",
                hosts, 1, 65);
}

/* ================= String returns ================= */

STRATA_TEST(extern_string_return_is_owned)
{
    /* The caller owns the returned buffer and frees it at scope exit
       (no double-free / leak with two live results). */
    HostSymbol hosts[] = { { "host_str_make", (void*)&HostStrMake }, { "host_str_len", (void*)&HostStrLen } };
    CheckExtern("extern string host_str_make();\n"
                "extern int host_str_len(string s);\n"
                "int entry()\n"
                "{\n"
                "  string a = host_str_make();\n"    /* owns \"fromhost\" (8 chars) */
                "  string b = host_str_make();\n"
                "  return host_str_len(a) + host_str_len(b);\n" /* 8 + 8 = 16 */
                "}\n",
                hosts, 2, 16);
}

STRATA_TEST(extern_string_param_and_return)
{
    /* Params arrive by value and stay borrowed; the returned string is
       owned and read back through another extern call. */
    HostSymbol hosts[] = { { "host_str_concat", (void*)&HostStrConcat }, { "host_str_len", (void*)&HostStrLen } };
    CheckExtern("extern string host_str_concat(string a, string b);\n"
                "extern int host_str_len(string s);\n"
                "int entry()\n"
                "{\n"
                "  string a = \"foo\";\n"
                "  string b = \"bar\";\n"
                "  string c = host_str_concat(a, b);\n" /* owns \"foobar\" */
                "  return host_str_len(a) + host_str_len(b) + host_str_len(c);\n" /* 3 + 3 + 6 = 12 */
                "}\n",
                hosts, 2, 12);
}

/* ================= Array params ================= */

STRATA_TEST(extern_int_array_param_borrows)
{
    /* An extern owned `int[]` param is passed by address of the slot and
       never moves the caller's array. */
    HostSymbol hosts[] = { { "host_arr_sum", (void*)&HostArrSum } };
    CheckExtern("extern int host_arr_sum(int[] a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a = {1, 2, 3};\n"
                "  int s = host_arr_sum(a, (int)a.length);\n"       /* 6; a borrowed, still live */
                "  return s + (int)a.length;\n"      /* 6 + 3 = 9 */
                "}\n",
                hosts, 1, 9);
}

STRATA_TEST(extern_ref_int_array_mutates_elements)
{
    HostSymbol hosts[] = { { "host_arr_bump", (void*)&HostArrBump }, { "host_arr_sum", (void*)&HostArrSum } };
    CheckExtern("extern void host_arr_bump(ref int[] a);\n"
                "extern int host_arr_sum(int[] a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a = {1, 2, 3};\n"
                "  host_arr_bump(a);\n"              /* {2,3,4} */
                "  host_arr_bump(a);\n"              /* {3,4,5} */
                "  return host_arr_sum(a, (int)a.length);\n"        /* 12 */
                "}\n",
                hosts, 2, 12);
}

STRATA_TEST(extern_ref_int_array_out_param)
{
    /* A `ref int[]` host can replace the whole array; the caller then owns
       (and frees) the buffer the host allocated. */
    HostSymbol hosts[] = { { "host_arr_fill", (void*)&HostArrFill }, { "host_arr_sum", (void*)&HostArrSum } };
    CheckExtern("extern void host_arr_fill(ref int[] a, int n);\n"
                "extern int host_arr_sum(int[] a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a;\n"
                "  host_arr_fill(a, 3);\n"           /* a = {0, 10, 20} */
                "  return host_arr_sum(a, (int)a.length) + (int)a.length;\n" /* 30 + 3 = 33 */
                "}\n",
                hosts, 2, 33);
}

STRATA_TEST(extern_string_array_param_count)
{
    /* A string[] (owning elements) crosses as the fat struct; reading its
       length from the host and freeing the strings at scope exit must be
       clean. */
    HostSymbol hosts[] = { { "host_str_arr_count", (void*)&HostStrArrCount } };
    CheckExtern("extern int host_str_arr_count(string[] a);\n"
                "int entry()\n"
                "{\n"
                "  string[] a = {\"aa\", \"bb\", \"cc\"};\n"
                "  return host_str_arr_count(a);\n"  /* 3 */
                "}\n",
                hosts, 1, 3);
}

/* ================= Array decay to ^T params =================

   An extern `^T` param (host sees a plain `T*`) accepts a dynamic `T[]` or a
   fixed `T[N]` struct field by decaying to a pointer to its FIRST element.
   The host reads/mutates the caller's actual buffer/field storage. */

STRATA_TEST(extern_hat_dyn_float_array_decays_to_ptr)
{
    /* float[] -> ^float: the data buffer pointer crosses, length as the
       caller-supplied arg. */
    HostSymbol hosts[] = { { "host_float_sum", (void*)&HostFloatSum } };
    CheckExtern("extern int host_float_sum(^float a, int n);\n"
                "int entry()\n"
                "{\n"
                "  float[] a = {1.0, 2.0, 3.0, 4.0};\n"
                "  return host_float_sum(a, (int)a.length);\n"   /* 10 */
                "}\n",
                hosts, 1, 10);
}

STRATA_TEST(extern_hat_fixed_float16_field_decays_to_ptr)
{
    /* float[16] struct field -> ^float: GEP to element 0 of the inline
       storage (short rows zero-fill, so 1..5 = 15). */
    HostSymbol hosts[] = { { "host_float_sum", (void*)&HostFloatSum } };
    CheckExtern("extern int host_float_sum(^float a, int n);\n"
                "struct Buf { float[16] data; };\n"
                "int entry()\n"
                "{\n"
                "  Buf b = { .data = {1.0, 2.0, 3.0, 4.0, 5.0} };\n"
                "  return host_float_sum(b.data, (int)b.data.length);\n"   /* 15 */
                "}\n",
                hosts, 1, 15);
}

STRATA_TEST(extern_hat_dyn_int_array_host_mutates_in_place)
{
    /* The decay passes the caller's live buffer, so a host can mutate it and
       Strata reads the changes back (externs borrow - nothing is moved). */
    HostSymbol hosts[] = { { "host_int_bump", (void*)&HostIntBump } };
    CheckExtern("extern void host_int_bump(^int a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a = {1, 2, 3};\n"
                "  host_int_bump(a, (int)a.length);\n"   /* {2,3,4} */
                "  return a[0] + a[1] + a[2];\n"         /* 9 */
                "}\n",
                hosts, 1, 9);
}

STRATA_TEST(extern_hat_fixed_int8_field_host_mutates_in_place)
{
    /* Fixed array field -> ^int: the pointer hits the struct's inline
       storage, so the host mutates the actual field, not a copy. */
    HostSymbol hosts[] = { { "host_int_bump", (void*)&HostIntBump } };
    CheckExtern("extern void host_int_bump(^int a, int n);\n"
                "struct Buf { int[8] data; };\n"
                "int entry()\n"
                "{\n"
                "  Buf b = { .data = {1, 2, 3} };\n"
                "  host_int_bump(b.data, 8);\n"          /* {2,3,4,1,1,1,1,1} */
                "  return b.data[0] + b.data[1] + b.data[2];\n"   /* 9 */
                "}\n",
                hosts, 1, 9);
}

STRATA_TEST(extern_hat_dyn_array_borrowed_not_moved)
{
    /* After the decayed call the array is still live and readable - the
       extern never consumes it. */
    HostSymbol hosts[] = { { "host_int_sum", (void*)&HostIntSum } };
    CheckExtern("extern int host_int_sum(^int a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a = {5, 6, 7};\n"
                "  int s = host_int_sum(a, (int)a.length);\n"   /* 18 */
                "  return s + (int)a.length + a[0];\n"          /* 18 + 3 + 5 = 26 */
                "}\n",
                hosts, 1, 26);
}

STRATA_TEST(extern_hat_double_array_decays_to_ptr)
{
    /* double[] -> ^double: doubles cross exactly (the element pointer is a
       double*, matching C). */
    HostSymbol hosts[] = { { "host_double_sum", (void*)&HostDoubleSum } };
    CheckExtern("extern int host_double_sum(^double a, int n);\n"
                "int entry()\n"
                "{\n"
                "  double[] a = {1.5, 2.5, 3.0};\n"
                "  return host_double_sum(a, (int)a.length);\n"   /* 7 */
                "}\n",
                hosts, 1, 7);
}

STRATA_TEST(extern_hat_byte_fixed_field_decays_to_ptr)
{
    /* byte[4] field -> ^byte: sums the unsigned bytes. */
    HostSymbol hosts[] = { { "host_byte_sum", (void*)&HostByteSum } };
    CheckExtern("extern int host_byte_sum(^byte a, int n);\n"
                "struct Buf { byte[4] data; };\n"
                "int entry()\n"
                "{\n"
                "  Buf b = { .data = {10, 20, 30, 40} };\n"
                "  return host_byte_sum(b.data, 4);\n"   /* 100 */
                "}\n",
                hosts, 1, 100);
}

STRATA_TEST(extern_hat_two_array_params_in_one_call)
{
    /* Two decayed array params (^float and ^int) in one extern call - each
       arg decays independently. */
    HostSymbol hosts[] = { { "host_float_int_mix", (void*)&HostFloatIntMix } };
    CheckExtern("extern int host_float_int_mix(^float f, int fn, ^int i, int in);\n"
                "int entry()\n"
                "{\n"
                "  float[] f = {3.0, 4.0};\n"
                "  int[] i = {7, 8};\n"
                "  return host_float_int_mix(f, (int)f.length, i, (int)i.length);\n"   /* 3 + 7 + 2 + 2 = 14 */
                "}\n",
                hosts, 1, 14);
}

STRATA_TEST(extern_hat_host_fills_dyn_buffer)
{
    /* The host writes through the decayed pointer into the caller's buffer;
       Strata reads the filled values back. */
    HostSymbol hosts[] = { { "host_int_fill", (void*)&HostIntFill } };
    CheckExtern("extern void host_int_fill(^int a, int n);\n"
                "int entry()\n"
                "{\n"
                "  int[] a = {0, 0, 0, 0};\n"
                "  host_int_fill(a, (int)a.length);\n"   /* {0,2,4,6} */
                "  return a[0] + a[1] + a[2] + a[3];\n"  /* 12 */
                "}\n",
                hosts, 1, 12);
}

STRATA_TEST(extern_hat_fixed_and_dyn_arrays_together)
{
    /* Fixed field and dynamic array both decay in the same function. */
    HostSymbol hosts[] = { { "host_float_sum", (void*)&HostFloatSum } };
    CheckExtern("extern int host_float_sum(^float a, int n);\n"
                "struct Buf { float[4] data; };\n"
                "int entry()\n"
                "{\n"
                "  Buf b = { .data = {1.0, 2.0, 3.0, 4.0} };\n"
                "  float[] a = {10.0, 20.0};\n"
                "  return host_float_sum(b.data, 4) + host_float_sum(a, (int)a.length);\n"   /* 10 + 30 = 40 */
                "}\n",
                hosts, 1, 40);
}

/* ================= Array returns ================= */

/* NOTE: an extern `int[]` RETURN is not exercised here. Returning the
   16-byte {data, len} struct across the host boundary is ABI-host-dependent
   (hidden sret pointer vs register return), and the only backend that
   matched this build's MinGW host was TinyCC, which has been removed. */

/* ================= Box params ================= */

STRATA_TEST(extern_box_struct_param_borrows)
{
    /* ^T crosses as the box cell pointer; extern never moves the caller's
       box, so it stays live afterward. */
    HostSymbol hosts[] = { { "host_box_read", (void*)&HostBoxRead } };
    CheckExtern("struct Foo { int v; };\n"
                "extern int host_box_read(^Foo b);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo b = Foo { .v = 7 };\n"
                "  int r = host_box_read(b);\n"      /* 7; b borrowed, still live */
                "  return r + b.v;\n"                /* 7 + 7 = 14 */
                "}\n",
                hosts, 1, 14);
}

STRATA_TEST(extern_box_struct_param_host_mutates)
{
    HostSymbol hosts[] = { { "host_box_set", (void*)&HostBoxSet } };
    CheckExtern("struct Foo { int v; };\n"
                "extern void host_box_set(^Foo b);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo b = Foo { .v = 1 };\n"
                "  host_box_set(b);\n"               /* sets b.v = 99 in place */
                "  return b.v;\n"                    /* 99 */
                "}\n",
                hosts, 1, 99);
}

STRATA_TEST(extern_ref_box_struct_param)
{
    HostSymbol hosts[] = { { "host_box_read_ref", (void*)&HostBoxReadRef } };
    CheckExtern("struct Foo { int v; };\n"
                "extern int host_box_read_ref(ref ^Foo b);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo b = Foo { .v = 5 };\n"
                "  int r = host_box_read_ref(b);\n"  /* 5 */
                "  return r + b.v;\n"                /* 5 + 5 = 10 */
                "}\n",
                hosts, 1, 10);
}

STRATA_TEST(extern_box_string_param)
{
    /* ^string crosses as the cell pointer (char**): one deref reaches
       the string content.
       Runs LLVM-only for now: the TCC leg miscompiles this case and TCC
       is slated for removal. */
    HostSymbol hosts[] = { { "host_str_box_len", (void*)&HostStrBoxLen } };
    CheckLlvmOnlyExtern("extern int host_str_box_len(^string b);\n"
                        "int entry()\n"
                        "{\n"
                        "  ^string b = \"hello\";\n"
                        "  return host_str_box_len(b);\n"    /* 5; b still live */
                        "}\n",
                        hosts, 1, 5);
}

STRATA_TEST(extern_box_struct_array_param)
{
    /* ^Foo[] (owning elements) crosses as the fat struct; the host
       reaches the first boxed element. The array stays live after. */
    HostSymbol hosts[] = { { "host_box_arr_first", (void*)&HostBoxArrFirst } };
    CheckExtern("struct Foo { int v; };\n"
                "extern int host_box_arr_first(^Foo[] arr);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo[] arr = { Foo { .v = 9 }, Foo { .v = 8 } };\n"
                "  int f = host_box_arr_first(arr);\n" /* 9; arr borrowed */
                "  return f + arr[1].v;\n"            /* 9 + 8 = 17 */
                "}\n",
                hosts, 1, 17);
}

/* ================= Box returns ================= */

STRATA_TEST(extern_box_struct_return_is_owned)
{
    /* The caller owns the returned box slot and frees it at scope exit. */
    HostSymbol hosts[] = { { "host_box_make", (void*)&HostBoxMake } };
    CheckExtern("struct Foo { int v; };\n"
                "extern ^Foo host_box_make(int v);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo a = host_box_make(10);\n" /* owns a heap Foo */
                "  ^Foo b = host_box_make(32);\n"
                "  return a.v + b.v;\n"               /* 42 */
                "}\n",
                hosts, 1, 42);
}

/* ================= `return` out-params =================
   `extern void F(return T x)` is the out-param idiom for C functions that
   cannot return a struct by value: the Strata signature reads `T F()` and
   the caller writes `T x = F();`. The ABI is void ret + one pointer. */

static void HostFooGet(HostFoo* f)
{
    f->v = 42;
}

static void HostIntGet(int* v)
{
    *v = 7;
}

static void HostStrGet(HostArr* s)
{
    char* buf = HostDup("hoststr");
    s->data = buf;
    s->len = 7;
}

static void HostHandleGet(void** h)
{
    *h = (void*)0x1234;
}

STRATA_TEST(extern_return_param_struct)
{
    HostSymbol hosts[] = { { "host_foo_get", (void*)&HostFooGet } };
    CheckExtern("struct Foo { int v; };\n"
                "extern void host_foo_get(return Foo f);\n"
                "int entry()\n"
                "{\n"
                "  Foo f = host_foo_get();\n" /* Foo { .v = 42 } */
                "  return f.v;\n"             /* 42 */
                "}\n",
                hosts, 1, 42);
}

STRATA_TEST(extern_return_param_forward_declared_struct)
{
    /* A forward-declared struct may appear in the `return` param declaration;
       the definition (here anywhere in the module) is what the CALL needs. */
    HostSymbol hosts[] = { { "host_foo_get", (void*)&HostFooGet } };
    CheckExtern("struct Foo;\n"
                "extern void host_foo_get(return Foo f);\n"
                "struct Foo { int v; };\n"
                "int entry()\n"
                "{\n"
                "  Foo f = host_foo_get();\n" /* Foo { .v = 42 } */
                "  return f.v;\n"             /* 42 */
                "}\n",
                hosts, 1, 42);
}

STRATA_TEST(extern_return_param_struct_passed_to_ref)
{
    /* The returned value is a real local; it can be handed to another
       extern (by-ref struct param) and read again. */
    HostSymbol hosts[] = { { "host_foo_get", (void*)&HostFooGet }, { "host_box_read", (void*)&HostBoxRead } };
    CheckExtern("struct Foo { int v; };\n"
                "extern void host_foo_get(return Foo f);\n"
                "extern int host_box_read(Foo f);\n"
                "int entry()\n"
                "{\n"
                "  Foo f = host_foo_get();\n"   /* { .v = 42 } */
                "  int r = host_box_read(f);\n" /* 42 (by-ref struct param) */
                "  return r + f.v;\n"           /* 84 */
                "}\n",
                hosts, 2, 84);
}

STRATA_TEST(extern_return_param_scalar)
{
    HostSymbol hosts[] = { { "host_int_get", (void*)&HostIntGet } };
    CheckExtern("extern void host_int_get(return int v);\n"
                "int entry()\n"
                "{\n"
                "  int v = host_int_get();\n"
                "  return v + 1;\n" /* 8 */
                "}\n",
                hosts, 1, 8);
}

STRATA_TEST(extern_return_param_string)
{
    /* The callee writes the fat {ptr, len} into the out slot; the caller
       owns the buffer and frees it at scope exit (no leak). */
    HostSymbol hosts[] = { { "host_str_get", (void*)&HostStrGet }, { "host_str_len", (void*)&HostStrLen } };
    CheckExtern("extern void host_str_get(return string s);\n"
                "extern int host_str_len(string s);\n"
                "int entry()\n"
                "{\n"
                "  string s = host_str_get();\n" /* owns \"hoststr\" (7 chars) */
                "  return host_str_len(s);\n"     /* 7 */
                "}\n",
                hosts, 2, 7);
}

STRATA_TEST(extern_return_param_handle)
{
    HostSymbol hosts[] = { { "host_handle_get", (void*)&HostHandleGet } };
    CheckExtern("handle Entity;\n"
                "extern void host_handle_get(return Entity e);\n"
                "int entry()\n"
                "{\n"
                "  Entity e = host_handle_get();\n" /* e = 0x1234 */
                "  return 1;\n"
                "}\n",
                hosts, 1, 1);
}

static void HostFooBoxGet(HostFoo** f)
{
    HostFoo* p = (HostFoo*)malloc(sizeof(HostFoo));
    if (p)
    {
        p->v = 42;
    }
    *f = p;
}

STRATA_TEST(extern_return_param_box)
{
    /* `return ^Foo f` crosses as a single out-pointer (Foo**); the caller
       owns the box and frees it at scope exit. */
    HostSymbol hosts[] = { { "host_foo_box_get", (void*)&HostFooBoxGet } };
    CheckExtern("struct Foo { int v; };\n"
                "extern void host_foo_box_get(return ^Foo f);\n"
                "int entry()\n"
                "{\n"
                "  ^Foo f = host_foo_box_get();\n" /* owns a heap Foo { .v = 42 } */
                "  return f.v;\n"                  /* 42 */
                "}\n",
                hosts, 1, 42);
}

STRATA_TEST(extern_return_param_rejects_struct_by_value_return)
{
    /* The declared return must be `void`; a struct by value is rejected. */
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c,
                                         "struct Foo { int v; };\n"
                                         "extern Foo GetBad(return Foo f);\n"
                                         "int entry() { Foo f = GetBad(); return f.v; }",
                                         "bad_return_param", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "must declare 'void' return") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

/* ================= `impl` on structs ================= */

static void HostOpaqueGetType(void** out)
{
    int* p = (int*)malloc(sizeof(int));
    if (p)
    {
        *p = 42;
    }
    *out = p;
}

static void HostOpaqueSetValue(void* self, int v)
{
    *((int*)self) = v;
}

static int HostOpaqueBump(void* self)
{
    int old = *((int*)self);
    *((int*)self) = old + 1;
    return old;
}

static int HostOpaqueGetValue(void* self)
{
    return *((int*)self);
}

STRATA_TEST(impl_on_forward_declared_struct_via_box)
{
    /* `struct TheType;` is never defined: the host owns the layout, and the
       script drives it opaquely through a `^TheType` box (a `return` param)
       and impl-declared extern methods. */
    HostSymbol hosts[] = {
        { "host_get_type", (void*)&HostOpaqueGetType },
        { "TheType_GetValue", (void*)&HostOpaqueGetValue },
        { "TheType_SetValue", (void*)&HostOpaqueSetValue },
        { "TheType_Bump", (void*)&HostOpaqueBump },
    };
    CheckExtern("struct TheType;\n"
                "extern void host_get_type(return ^TheType t);\n"
                "impl TheType {\n"
                "    extern int GetValue(TheType self);\n"
                "    extern void SetValue(TheType self, int v);\n"
                "    extern int Bump(TheType self);\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  ^TheType t = host_get_type();\n" /* box { 42 } */
                "  int v = t.GetValue();\n"          /* 42 */
                "  t.SetValue(v + 1);\n"             /* host stores 43 */
                "  int w = t.Bump();\n"              /* reads 43, stores 44, returns 43 */
                "  return w + t.GetValue();\n"       /* 43 + 44 = 87 */
                "}\n",
                hosts, 4, 87);
}

typedef struct
{
    int a;
    int b;
} HostPair;

static int HostPairSum(HostPair* p)
{
    return p->a + p->b;
}

static int HostPairStaticSum(int a, int b)
{
    return a + b;
}

static int HostMeterGet(int self)
{
    return self + 1;
}

static int HostXGet(HostPair* self)
{
    return self->a * 10 + self->b;
}

static int HostMeterNew(void)
{
    return 1234;
}

STRATA_TEST(impl_on_defined_struct)
{
    /* `impl` also works on a fully defined struct: the self param crosses
       by reference like any struct parameter. */
    HostSymbol hosts[] = { { "Pair_Sum", (void*)&HostPairSum } };
    CheckExtern("struct Pair { int a; int b; };\n"
                "impl Pair {\n"
                "    extern int Sum(Pair self);\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  Pair p = { 3, 4 };\n"
                "  return p.Sum();\n" /* 7 */
                "}\n",
                hosts, 1, 7);
}

STRATA_TEST(impl_on_defined_struct_static_method)
{
    /* A parameterless-self impl method resolves as a static call. */
    HostSymbol hosts[] = { { "Pair_StaticSum", (void*)&HostPairStaticSum } };
    CheckExtern("struct Pair { int a; int b; };\n"
                "impl Pair {\n"
                "    extern int StaticSum(int a, int b);\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  return Pair.StaticSum(3, 4);\n" /* 7 */
                "}\n",
                hosts, 1, 7);
}

STRATA_TEST(impl_on_type_alias_scalar_jit)
{
    /* `impl` on a strong alias of a scalar: self crosses by value (i32). */
    HostSymbol hosts[] = { { "Meter_Get", (void*)&HostMeterGet } };
    CheckExtern("struct Meter = int;\n"
                "impl Meter {\n"
                "    extern int Get(Meter self);\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  Meter m = (Meter)41;\n"
                "  return m.Get();\n" /* 42 */
                "}\n",
                hosts, 1, 42);
}

STRATA_TEST(impl_on_type_alias_struct_jit)
{
    /* `impl` on a strong alias of a struct: self crosses by reference like
       any struct parameter. The alias is its own target — distinct from the
       underlying's impl (see impl_on_defined_struct). */
    HostSymbol hosts[] = { { "X_Get", (void*)&HostXGet } };
    CheckExtern("struct Pair { int a; int b; };\n"
                "struct X = Pair;\n"
                "impl X {\n"
                "    extern int Get(X self);\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  Pair p = { 3, 4 };\n"
                "  X x = (X)p;\n"
                "  return x.Get();\n" /* 34 */
                "}\n",
                hosts, 1, 34);
}

STRATA_TEST(impl_on_type_alias_static_factory_jit)
{
    /* A parameterless impl method on an alias resolves as a static call and
       returns the underlying scalar by value. */
    HostSymbol hosts[] = { { "Meter_New", (void*)&HostMeterNew } };
    CheckExtern("struct Meter = int;\n"
                "impl Meter {\n"
                "    extern Meter New();\n"
                "}\n"
                "int entry()\n"
                "{\n"
                "  Meter m = Meter.New();\n"
                "  return (int)m;\n" /* 1234 */
                "}\n",
                hosts, 1, 1234);
}

/* ---- Strong typedef (type alias) ABI ---- */

static uint32_t HostStateGet(uint32_t s)
{
    return s;
}

static uint32_t HostStateReturn(uint32_t s)
{
    return s;
}

static int HostMix(uint32_t a, int b, uint32_t c)
{
    return (a == 1 && b == 2 && c == 3) ? 1 : 0;
}

static void HostStateInc(uint32_t* s)
{
    *s += 1;
}

static void* HostMakeEntity(void)
{
    return (void*)(uintptr_t)0x1234;
}

static int HostCheckHandle(void* h)
{
    return h == (void*)(uintptr_t)0x1234 ? 1 : 0;
}

typedef struct
{
    float x;
    float y;
    float z;
} HostVec3;

static int HostPointY(HostVec3* p)
{
    return (int)p->y;
}

STRATA_TEST(extern_alias_scalar_param)
{
    /* A strong typedef (`struct State = uint`) crosses the extern ABI as the
       underlying scalar by VALUE (i32), not as a pointer to the caller's
       slot. Regression: aliases were classified as structs in
       DeclareFunction/DefineFunction, so the host received &s. */
    HostSymbol hosts[] = { { "host_state_get", (void*)&HostStateGet } };
    CheckExtern("struct State = uint;\n"
                "extern uint host_state_get(State s);\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)12345;\n"
                "  uint r = host_state_get(s);\n"
                "  if (r == 12345) { return 1; }\n"
                "  return 0;\n"
                "}\n",
                hosts, 1, 1);
}

STRATA_TEST(extern_alias_scalar_return)
{
    /* Alias return types cross as the underlying scalar value too. */
    HostSymbol hosts[] = { { "host_state_return", (void*)&HostStateReturn } };
    CheckExtern("struct State = uint;\n"
                "extern State host_state_return(State s);\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)77;\n"
                "  State r = host_state_return(s);\n"
                "  if (r == (State)77) { return 1; }\n"
                "  return 0;\n"
                "}\n",
                hosts, 1, 1);
}

STRATA_TEST(extern_alias_param_alias_of_alias)
{
    HostSymbol hosts[] = { { "host_state_get", (void*)&HostStateGet } };
    CheckExtern("struct State = uint;\n"
                "struct State2 = State;\n"
                "extern uint host_state_get(State2 s);\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)9;\n"
                "  uint r = host_state_get((State2)s);\n"
                "  if (r == 9) { return 1; }\n"
                "  return 0;\n"
                "}\n",
                hosts, 1, 1);
}

STRATA_TEST(extern_alias_param_mixed_args)
{
    /* Alias and primitive params interleaved each land in the right slot. */
    HostSymbol hosts[] = { { "host_mix", (void*)&HostMix } };
    CheckExtern("struct State = uint;\n"
                "extern int host_mix(State a, int b, State c);\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)3;\n"
                "  return host_mix((State)1, 2, s);\n"
                "}\n",
                hosts, 1, 1);
}

STRATA_TEST(extern_alias_param_ref)
{
    /* `ref State` stays a pointer to the caller's slot and mutates in place. */
    HostSymbol hosts[] = { { "host_state_inc", (void*)&HostStateInc } };
    CheckExtern("struct State = uint;\n"
                "extern void host_state_inc(ref State s);\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)10;\n"
                "  host_state_inc(s);\n"
                "  if (s == (State)11) { return 1; }\n"
                "  return 0;\n"
                "}\n",
                hosts, 1, 1);
}

STRATA_TEST(extern_alias_handle_param)
{
    /* An alias of a handle crosses by value like the handle itself. */
    HostSymbol hosts[] = {
        { "host_make_entity", (void*)&HostMakeEntity },
        { "host_check_handle", (void*)&HostCheckHandle },
    };
    CheckExtern("handle Entity;\n"
                "struct E2 = Entity;\n"
                "extern Entity host_make_entity();\n"
                "extern int host_check_handle(E2 h);\n"
                "int entry()\n"
                "{\n"
                "  E2 e = (E2)host_make_entity();\n"
                "  return host_check_handle(e);\n"
                "}\n",
                hosts, 2, 1);
}

STRATA_TEST(extern_alias_struct_param_still_by_ref)
{
    /* An alias of a DEFINED struct is still a struct at the ABI: by pointer. */
    HostSymbol hosts[] = { { "host_point_y", (void*)&HostPointY } };
    CheckExtern("struct Vec3 { float x; float y; float z; };\n"
                "struct Point = Vec3;\n"
                "extern int host_point_y(Point p);\n"
                "int entry()\n"
                "{\n"
                "  Point p = (Point)Vec3 { 1.0, 5.0, 3.0 };\n"
                "  return host_point_y(p);\n"
                "}\n",
                hosts, 1, 5);
}

STRATA_TEST(internal_alias_scalar_param)
{
    /* The same classification applies to DEFINED functions: a scalar-alias
       param is a by-value i32 slot, so reads operate on the local copy. */
    CheckExtern("struct State = uint;\n"
                "int twice(State s) { return (int)s * 2; }\n"
                "int entry()\n"
                "{\n"
                "  State s = (State)21;\n"
                "  int r = twice(s);\n"
                "  if (r == 42) { return 1; }\n"
                "  return 0;\n"
                "}\n",
                NULL, 0, 1);
}
