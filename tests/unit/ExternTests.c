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
