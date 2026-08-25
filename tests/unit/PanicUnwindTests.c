#include "strata/strata.h"

#include "Test.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* JIT panic unwinding (LLVM JIT only): strata_panic must unwind the JIT'd
   stack frame by frame — each frame's landing pad frees the owning values it
   holds (box<T> / string / T[], including rest-param elements) — until the
   host boundary wrapper is reached. The panic handler then runs once, after
   the unwind, with the TLS unwind chain restored; if it returns, the entry
   function returns a zeroed value so the host process survives. */

static jmp_buf  s_panicJmp;
static char     s_panicMsg[256];
static int      s_panicCount;
static long     s_allocs;
static long     s_frees;
static StrataJit* s_jit;

static void* CountAlloc(unsigned long n)
{
    s_allocs++;
    return malloc((size_t)n);
}

static void CountFree(void* p)
{
    s_frees++;
    free(p);
}

/* Records the message and RETURNS: with unwinding active the entry function
   must come back with a zeroed return instead of crashing. */
static void RecordPanicHandler(const char* msg)
{
    s_panicCount++;
    snprintf(s_panicMsg, sizeof s_panicMsg, "%s", msg ? msg : "(null)");
}

/* Longjmps straight out of the panic notification (the legacy host pattern).
   With unwinding active the longjmp happens at the boundary after the TLS
   chain is restored, so it must stay safe. */
static void LongJmpPanicHandler(const char* msg)
{
    s_panicCount++;
    snprintf(s_panicMsg, sizeof s_panicMsg, "%s", msg ? msg : "(null)");
    longjmp(s_panicJmp, 1);
}

static StrataJit* CompileLlvm(const char* source, int panicUnwind)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return NULL;
    }

    StrataProfile profile = strataProfileDefault();
    profile.panicUnwind = (unsigned)panicUnwind;

    StrataCompiler* c = strataCompilerCreate();
    strataJitSetBackend(c, STRATA_JIT_BACKEND_LLVM);
    strataJitSetProfile(c, &profile);
    strataJitSetAllocFreeFunctions(c, (void*)&CountAlloc, (void*)&CountFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, source, "panicunwind", &err);

    strataFree((char*)err);
    strataCompilerDestroy(c);
    return jit;
}

static void ResetCounters(void)
{
    s_panicCount = 0;
    s_panicMsg[0] = '\0';
    s_allocs = 0;
    s_frees = 0;
}

/* A panic deep in a chain of owning frames must free every heap value each
   frame owns (local arrays, boxes, moved-in strings), call the host handler
   exactly once at the boundary, and return 0 to the host. */
STRATA_TEST(panic_unwind_frees_owning_frames)
{
    StrataJit* jit = CompileLlvm(
        "string make_it() {\n"
        "  string s = \"created\";\n"
        "  int[] junk = {1, 2};\n"
        "  return s;\n"
        "}\n"
        "int middle() {\n"
        "  string s = make_it();\n"
        "  box<int> b = 5;\n"
        "  return boom();\n"
        "}\n"
        "int boom() {\n"
        "  int[] a = {1, 2, 3};\n"
        "  return a[9];\n"
        "}\n"
        "int entry() { return middle(); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        STRATA_CHECK(s_allocs > 0);
    }

    strataJitDestroy(jit);
}

/* The host must survive and keep using the same module: after a panic the
   entry boundary (and the whole TLS unwind chain) resets, so a second call
   works normally and a second panic is caught again. */
STRATA_TEST(panic_unwind_host_survives_repeated_panics)
{
    StrataJit* jit = CompileLlvm(
        "int inner() { int[] a = {1, 2}; return a[7]; }\n"
        "int entry() { int[] keep = {42}; return inner() + keep[0]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r1 = entry(); /* panics: inner's array + entry's array freed */
        int r2 = entry(); /* must still work; panics again */

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r1, 0);
        STRATA_CHECK_EQ(r2, 0);
        STRATA_CHECK_EQ(s_panicCount, 2);
        STRATA_CHECK_EQ(s_allocs, s_frees);
    }

    strataJitDestroy(jit);
}

/* A panic inside a var-decl initializer (before the slot's initializer
   store) must not make the landing pad drop an uninitialized slot: owning
   slots are nulled at alloca time in unwind mode. */
STRATA_TEST(panic_unwind_during_initializer_slot_is_nulled)
{
    StrataJit* jit = CompileLlvm(
        "string boom() {\n"
        "  int[] a = {1, 2};\n"
        "  int x = a[9];\n"
        "  return \"never\";\n"
        "}\n"
        "int entry() { string s = boom(); return 0; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
    }

    strataJitDestroy(jit);
}

/* Host callback re-entering the JIT: an inner invocation that panics must
   unwind only to its own boundary. The outer invocation continues with its
   owning locals intact, and the unwind chain is balanced. */
static int HostReEnter(int x)
{
    int (*inner)(void) = (int (*)(void))strataJitGetFunction(s_jit, "inner");

    if (!inner)
    {
        return -1000;
    }

    int r = inner(); /* panics inside; unwinds to inner's own boundary */
    return 41 + r;
}

STRATA_TEST(panic_unwind_reentrancy_from_host_callback)
{
    StrataJit* jit = CompileLlvm(
        "extern int host_cb(int x);\n"
        "int inner() { int[] a = {1, 2}; return a[5]; }\n"
        "int entry() { int[] keep = {7}; return host_cb(1) + keep[0]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    s_jit = jit;

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_cb", (void*)&HostReEnter), 1);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry(); /* 41 + 7 = 48; the inner panic unwound only itself */

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 48);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
    }

    s_jit = NULL;
    strataJitDestroy(jit);
}

/* Owning rest-param elements (string... rest) live in a caller-owned stack
   buffer; the callee's landing pad must drop the elements (freeing their
   heap copies) without freeing the buffer itself. */
STRATA_TEST(panic_unwind_drops_rest_param_elements)
{
    StrataJit* jit = CompileLlvm(
        "int deep(string... rest) {\n"
        "  int[] a = {1};\n"
        "  int x = a[4];\n"
        "  return 0;\n"
        "}\n"
        "int entry() { return deep(\"aa\", \"bb\"); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        STRATA_CHECK(s_allocs > 0);
    }

    strataJitDestroy(jit);
}

/* A longjmping panic handler (the classic embed pattern) must still work:
   with unwinding on, the handler is notified at the boundary after the TLS
   chain is restored, so its longjmp leaves no stale unwind state behind.
   Windows requires the boundary wrapper's unwind info to be registered with
   the OS (TCC wrapper, or the native COFF wrapper on LLVM-only builds) so
   the longjmp's RtlUnwind can walk it. */
STRATA_TEST(panic_unwind_longjmp_handler_still_works)
{
    StrataJit* jit = CompileLlvm(
        "int entry() { int[] a = {1, 2, 3}; return a[5]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        s_panicCount = 0;
        s_panicMsg[0] = '\0';
        strataSetPanicHandler(LongJmpPanicHandler);

        if (setjmp(s_panicJmp) == 0)
        {
            entry();
            STRATA_CHECK(0 && "expected a panic but entry() returned");
        }

        strataSetPanicHandler(NULL);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
    }

    strataJitDestroy(jit);
}

/* The MSVC-host scenario: ucrtbase's longjmp always unwinds via RtlUnwind
   (MinGW's msvcrt longjmp is a lenient register restore, so the plain
   longjmp test above cannot prove the RtlUnwind path). Simulates an MSVC
   engine whose panic handler longjmps across the boundary wrapper: without
   OS-registered unwind info for the wrapper this is exactly the
   STATUS_BAD_STACK (0xC0000028) crash seen in the wild. */
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
static int (*UcrtSetJmp)(void*, void*);
static void (*UcrtLongJmp)(void*, int);
static __attribute__((aligned(16))) unsigned char s_ucrtJmp[512]; /* ucrt x64 jmp_buf is 256 bytes */

static void UcrtLongJmpHandler(const char* msg)
{
    s_panicCount++;
    snprintf(s_panicMsg, sizeof s_panicMsg, "%s", msg ? msg : "(null)");
    UcrtLongJmp(s_ucrtJmp, 1);
}

STRATA_TEST(panic_unwind_rtlunwind_longjmp_across_wrapper)
{
    HMODULE ucrt = LoadLibraryA("ucrtbase.dll");

    if (!ucrt)
    {
        return; /* pre-Windows-10 host: nothing to prove */
    }

    UcrtSetJmp = (int (*)(void*, void*))(void*)GetProcAddress(ucrt, "setjmp");
    UcrtLongJmp = (void (*)(void*, int))(void*)GetProcAddress(ucrt, "longjmp");

    if (!UcrtSetJmp || !UcrtLongJmp)
    {
        return;
    }

    StrataJit* jit = CompileLlvm(
        "int entry() { int[] a = {1, 2, 3}; return a[5]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        s_panicCount = 0;
        s_panicMsg[0] = '\0';
        memset(s_ucrtJmp, 0, sizeof s_ucrtJmp);
        strataSetPanicHandler(UcrtLongJmpHandler);

        if (UcrtSetJmp(s_ucrtJmp, NULL) == 0)
        {
            entry();
            STRATA_CHECK(0 && "expected a panic but entry() returned");
        }

        strataSetPanicHandler(NULL);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
    }

    strataJitDestroy(jit);
}
#endif

/* Flag-and-return pattern: a handler that returns (or polling without any
   handler installed) can ask strataConsumePanic after the entry function
   comes back zeroed. Returns 1 exactly once per panic, with the message. */
STRATA_TEST(panic_unwind_consume_panic_reports_each_panic_once)
{
    StrataJit* jit = CompileLlvm(
        "int inner() { int[] a = {1, 2}; return a[7]; }\n"
        "int entry() { int[] keep = {42}; return inner() + keep[0]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        const char* msg = "sentinel";

        /* earlier tests on this thread may have left panics pending: drain */
        while (strataConsumePanic(&msg))
        {
        }

        msg = "sentinel";
        STRATA_CHECK_EQ(strataConsumePanic(&msg), 0);
        STRATA_CHECK(msg == NULL);

        strataSetPanicHandler(RecordPanicHandler);

        int r1 = entry(); /* panics */
        int r2 = entry(); /* panics again */

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r1, 0);
        STRATA_CHECK_EQ(r2, 0);

        msg = NULL;
        STRATA_CHECK_EQ(strataConsumePanic(&msg), 1);
        STRATA_CHECK(msg != NULL && strstr(msg, "array index out of bounds") != NULL);
        STRATA_CHECK_EQ(strataConsumePanic(&msg), 0); /* consumed exactly once */

        /* ...and re-arms for the next panic (a handler must stay installed:
           without one the boundary still aborts after notifying) */
        strataSetPanicHandler(RecordPanicHandler);
        r1 = entry();
        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r1, 0);
        STRATA_CHECK_EQ(strataConsumePanic(&msg), 1);
        STRATA_CHECK_EQ(s_panicCount, 3);
    }

    strataJitDestroy(jit);
}

/* A void entry that panics must also return cleanly through its boundary
   wrapper (ret void on both paths). */
STRATA_TEST(panic_unwind_void_entry)
{
    StrataJit* jit = CompileLlvm(
        "void boom() { int[] a = {1}; int x = a[3]; }\n"
        "void entry() { boom(); }\n",
        1);

    if (!jit)
    {
        return;
    }

    void (*entry)(void) = (void (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
    }

    strataJitDestroy(jit);
}

/* The address strataJitGetFunction hands out is the panic-unwind boundary:
   on Windows it MUST have OS-registered unwind info (.pdata via
   RtlAddFunctionTable — TCC wrapper or native COFF wrapper), or a host panic
   handler that longjmps (or a C++ exception) unwinds an unregistered frame
   and dies with STATUS_BAD_STACK (0xC0000028). Regression test for the
   "wrapper silently fell back to the JIT'd wrapper" class of bug. */
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
typedef void* (*RtlLookupFn)(unsigned long long, unsigned long long*, void*);

static void* GetProc(const char* lib, const char* name)
{
    HMODULE m = GetModuleHandleA(lib);
    return m ? (void*)GetProcAddress(m, name) : NULL;
}

STRATA_TEST(panic_unwind_entry_point_has_registered_unwind_info)
{
    StrataJit* jit = CompileLlvm(
        "int entry() { int[] a = {1, 2, 3}; return a[5]; }\n",
        1);

    if (!jit)
    {
        return;
    }

    void* fn = strataJitGetFunction(jit, "entry");
    STRATA_CHECK(fn != NULL);

    RtlLookupFn lookup = (RtlLookupFn)GetProc("ntdll.dll", "RtlLookupFunctionEntry");

    if (lookup)
    {
        unsigned long long imgBase = 0;
        void* unwindEntry = lookup((unsigned long long)(uintptr_t)fn, &imgBase, NULL);
        STRATA_CHECK(unwindEntry != NULL);
    }

    strataJitDestroy(jit);
}
#endif

/* panicUnwind=0 must preserve the legacy behavior: the handler runs at the
   panic site (it can longjmp out of JIT'd frames as before). */
STRATA_TEST(panic_unwind_disabled_legacy_site_handler)
{
#if !defined(_WIN32) || STRATA_TEST_HAS_TCC
    StrataJit* jit = CompileLlvm(
        "int entry() { int[] a = {1, 2, 3}; return a[5]; }\n",
        0);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        s_panicCount = 0;
        s_panicMsg[0] = '\0';
        strataSetPanicHandler(LongJmpPanicHandler);

        if (setjmp(s_panicJmp) == 0)
        {
            entry();
            STRATA_CHECK(0 && "expected a panic but entry() returned");
        }

        strataSetPanicHandler(NULL);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
    }

    strataJitDestroy(jit);
#endif
}

/* Unwinding must not disturb normal (non-panicking) execution: results pass
   through the boundary wrapper untouched. */
STRATA_TEST(panic_unwind_normal_path_unaffected)
{
    StrataJit* jit = CompileLlvm(
        "int helper(int x) { int[] a = {x}; return a[0] + 1; }\n"
        "int entry() { string s = \"ok\"; return helper(41); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 42);
        STRATA_CHECK_EQ(s_panicCount, 0);
        STRATA_CHECK_EQ(s_allocs, s_frees);
    }

    strataJitDestroy(jit);
}

/* Array-focused unwind coverage. */

/* Owning locals declared in NESTED scopes (loop bodies, if branches) must be
   dropped by the landing pad: m_owningLocals.count is scope-truncated at
   block exits, so the pad drops by the registration high-water mark instead.
   Iteration 1's array is freed by its scope-exit drop; iteration 2's array
   and the if-branch array are freed by entry's pad. */
STRATA_TEST(panic_unwind_array_in_nested_scopes)
{
    StrataJit* jit = CompileLlvm(
        "int entry() {\n"
        "  int i = 0;\n"
        "  while (i < 3) {\n"
        "    int[] a = {i, i + 1};\n"
        "    i = i + 1;\n"
        "    if (i == 2) {\n"
        "      int[] b = {9};\n"
        "      return b[9];\n" /* panic with a and b live in nested scopes */
        "    }\n"
        "  }\n"
        "  return 0;\n"
        "}\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        STRATA_CHECK_EQ(s_allocs, 3);
    }

    strataJitDestroy(jit);
}

/* A panic while evaluating an array literal must not leak the partially
   built value: the buffer is stored into the slot (and zero-filled) before
   element evaluation, so the pad frees the buffer plus the elements built
   so far, and reads null for the rest. */
STRATA_TEST(panic_unwind_array_mid_literal_construction)
{
    StrataJit* jit = CompileLlvm(
        "string boom() {\n"
        "  int[] a = {1};\n"
        "  int x = a[9];\n"
        "  return \"never\";\n"
        "}\n"
        "int entry() { string[] s = { \"kept\", boom(), \"never\" }; return 0; }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        /* buffer + "kept" heap copy + boom's array */
        STRATA_CHECK_EQ(s_allocs, 3);
    }

    strataJitDestroy(jit);
}

/* An array moved into a frame from a function return is owned by that frame;
   the pad frees it exactly once, and the moved-out source slot (nulled by
   the move) must not produce a stray free(null). */
STRATA_TEST(panic_unwind_array_moved_from_return)
{
    StrataJit* jit = CompileLlvm(
        "int[] make() { int[] a = {1, 2}; return a; }\n"
        "int boom() { int[] a = {5}; return a[9]; }\n"
        "int entry() { int[] a = make(); return boom(); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        STRATA_CHECK_EQ(s_allocs, 2);
    }

    strataJitDestroy(jit);
}

/* string[] elements are heap copies owned by the array; the pad drops each
   element and then frees the backing buffer. */
STRATA_TEST(panic_unwind_string_array_elements)
{
    StrataJit* jit = CompileLlvm(
        "int boom() { int[] a = {1}; return a[9]; }\n"
        "int entry() { string[] s = {\"aa\", \"bb\", \"cc\"}; return boom(); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        /* buffer + 3 heap-copied strings + boom's array */
        STRATA_CHECK_EQ(s_allocs, 5);
    }

    strataJitDestroy(jit);
}

/* box<S>[] of owning structs: the pad drops each element via the per-type
   struct drop helper, freeing the inner box then the element box. */
STRATA_TEST(panic_unwind_box_array_of_owning_structs)
{
    StrataJit* jit = CompileLlvm(
        "struct Owns { box<int> child; };\n"
        "int boom() { int[] a = {1}; return a[9]; }\n"
        "int entry() {\n"
        "  box<Owns>[] s = { Owns { .child = 1 }, Owns { .child = 2 } };\n"
        "  return boom();\n"
        "}\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        /* buffer + 2 element boxes + 2 inner boxes + boom's array */
        STRATA_CHECK_EQ(s_allocs, 6);
    }

    strataJitDestroy(jit);
}

/* Reassigning an array frees the old buffer at the assignment; the pad then
   frees the new one exactly once. */
STRATA_TEST(panic_unwind_array_reassigned_before_panic)
{
    StrataJit* jit = CompileLlvm(
        "int boom() { int[] a = {1}; return a[9]; }\n"
        "int entry() { int[] a = {1}; a = {2, 3}; return boom(); }\n",
        1);

    if (!jit)
    {
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        ResetCounters();
        strataSetPanicHandler(RecordPanicHandler);

        int r = entry();

        strataSetPanicHandler(NULL);

        STRATA_CHECK_EQ(r, 0);
        STRATA_CHECK_EQ(s_panicCount, 1);
        STRATA_CHECK_EQ(s_allocs, s_frees);
        STRATA_CHECK_EQ(s_allocs, 3);
    }

    strataJitDestroy(jit);
}

