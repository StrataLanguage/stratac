#include "strata/strata.h"

#include "Test.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   Windows note: this requires the TCC-compiled boundary wrapper (unwind info
   registered via RtlAddFunctionTable), so the test is skipped on
   LLVM-only builds. */
STRATA_TEST(panic_unwind_longjmp_handler_still_works)
{
#if !defined(_WIN32) || STRATA_TEST_HAS_TCC
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
#endif
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
