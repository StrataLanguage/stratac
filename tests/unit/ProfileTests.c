#include "strata/strata.h"

#include "Test.h"
#include "Util.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf  s_panicJmp;
static char     s_panicMsg[256];
static int      s_panicCount;

static void TestPanicHandler(const char* msg)
{
    s_panicCount++;
    snprintf(s_panicMsg, sizeof s_panicMsg, "%s", msg ? msg : "(null)");
    longjmp(s_panicJmp, 1);
}

/* Non-jumping recorder: recoverable violations (JIT OOB) call the handler
   but execution continues, so the tests must record without unwinding. */
static void TestReportHandler(const char* msg)
{
    s_panicCount++;
    snprintf(s_panicMsg, sizeof s_panicMsg, "%s", msg ? msg : "(null)");
}

/* JIT-compiles `source` with `backend` and a profile where every check field
   is set to `checksOn` (1 = all on, 0 = all off). Returns a live StrataJit*
   (or NULL on failure) with the panic handler armed around the run. */
static StrataJit* CompileJit(const char* source, StrataJitBackend backend, int checksOn)
{
    StrataProfile profile;
    profile.boundsCheck = (unsigned)checksOn;
    profile.nullExternCall = (unsigned)checksOn;

    StrataCompiler* c = strataCompilerCreate();
    strataJitSetBackend(c, backend);
    strataJitSetProfile(c, &profile);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, source, "profile", &err);

    strataFree((char*)err);
    strataCompilerDestroy(c);
    return jit;
}

/* Calls `entry()` expecting a panic; verifies the recorded message contains
   `expectMsg`. */
static void ExpectPanic(StrataJit* jit, const char* expectMsg)
{
    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (!entry)
    {
        return;
    }

    s_panicCount = 0;
    s_panicMsg[0] = '\0';

    strataSetPanicHandler(TestPanicHandler);

    if (setjmp(s_panicJmp) == 0)
    {
        entry();
        strataSetPanicHandler(NULL);
        STRATA_CHECK(0 && "expected a panic but entry() returned");
        return;
    }

    strataSetPanicHandler(NULL);
    STRATA_CHECK_EQ(s_panicCount, 1);
    STRATA_CHECK(strstr(s_panicMsg, expectMsg) != NULL);
}

/* Runs `source` to completion and asserts it does NOT panic (all checks off). */
static void ExpectNoPanic(StrataJit* jit)
{
    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (!entry)
    {
        return;
    }

    s_panicCount = 0;
    s_panicMsg[0] = '\0';

    strataSetPanicHandler(TestPanicHandler);

    if (setjmp(s_panicJmp) == 0)
    {
        entry();
        strataSetPanicHandler(NULL);
        STRATA_CHECK_EQ(s_panicCount, 0);
        return;
    }

    strataSetPanicHandler(NULL);
    STRATA_CHECK(0 && "did not expect a panic");
}

/* Calls `entry()` with a non-unwinding handler installed; returns the value
   and leaves the violation count in *violations (message in s_panicMsg). */
static int CallEntryReport(StrataJit* jit, int* violations)
{
    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (!entry)
    {
        *violations = -1;
        return -1;
    }

    s_panicCount = 0;
    s_panicMsg[0] = '\0';

    strataSetPanicHandler(TestReportHandler);
    int value = entry();
    strataSetPanicHandler(NULL);

    *violations = s_panicCount;
    return value;
}

static void RunPanicPair(StrataJitBackend backend, const char* source, const char* expectMsg)
{
    unsigned caps = strataCapabilities();

    if (backend == STRATA_JIT_BACKEND_LLVM && !(caps & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(source, backend, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        ExpectPanic(jit, expectMsg);
        strataJitDestroy(jit);
    }
}

/* An errored module must be rejected cleanly (NULL jit + message), never
   crash the compiler — a return that reads out a NULL box-inner used to
   pass NULL into TypeRegistryIsOwningStruct and crash mid-resolve. */
static void CheckErrorRejected(StrataJitBackend backend, const char* source, const char* expectMsg)
{
    unsigned caps = strataCapabilities();

    if (backend == STRATA_JIT_BACKEND_LLVM && !(caps & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataCompiler* c = strataCompilerCreate();
    strataJitSetBackend(c, backend);
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, source, "err", &err);

    STRATA_CHECK(jit == NULL);
    if (err)
    {
        STRATA_CHECK(strstr(err, expectMsg) != NULL);
    }

    strataFree((char*)err);
    strataCompilerDestroy(c);
}

STRATA_TEST(errored_module_rejected_cleanly_llvm)
{
    CheckErrorRejected(STRATA_JIT_BACKEND_LLVM,
                       "int[] g = { 1 };\n"
                       "string[] f() { return g; }\n",
                       "cannot return a value of type 'int[]'");
}

STRATA_TEST(errored_module_rejected_cleanly_string_mismatch)
{
    CheckErrorRejected(STRATA_JIT_BACKEND_LLVM,
                       "int f() { string s = \"x\"; return s; }\n",
                       "cannot return a value of type 'string'");
}

/* The LLVM JIT with boundsCheck on does not panic on out-of-bounds: reads
   return a dummy element instead. Scalars come back zeroed. */
STRATA_TEST(profile_oob_read_returns_zero_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit("int entry() { int[] a = {1, 2, 3}; return a[5]; }",
                                STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 1);
        STRATA_CHECK(strstr(s_panicMsg, "array index out of bounds") != NULL);
        strataJitDestroy(jit);
    }
}

/* Out-of-bounds writes are absorbed: the array contents stay intact. */
STRATA_TEST(profile_oob_write_is_noop_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit("int entry() { int[] a = {1, 2, 3}; a[9] = 42; return a[0] + a[1] + a[2]; }",
                                STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 6);
        STRATA_CHECK_EQ(violations, 1);
        strataJitDestroy(jit);
    }
}

/* OOB string reads yield an owned empty string (heap "", free-able). */
STRATA_TEST(profile_oob_string_read_returns_empty_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "extern ulong strlen(string s);\n"
        "int entry() { string[] s = {\"a\", \"b\"}; string t = s[9]; return (int)strlen(t); }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ(strataJitAddSymbol(jit, "strlen", (void*)&strlen), 1);
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 1);
        strataJitDestroy(jit);
    }
}

/* OOB string writes land in scratch; in-bounds elements are untouched. */
STRATA_TEST(profile_oob_string_write_noop_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "extern ulong strlen(string s);\n"
        "int entry() { string[] s = {\"a\", \"b\"}; s[9] = \"zz\"; return (int)(strlen(s[0]) + strlen(s[1])); }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ(strataJitAddSymbol(jit, "strlen", (void*)&strlen), 1);
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 2);
        STRATA_CHECK_EQ(violations, 1);
        strataJitDestroy(jit);
    }
}

/* OOB ^struct reads give a newly constructed (zeroed) struct. */
STRATA_TEST(profile_oob_box_struct_field_zero_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "struct W { int dmg; };\n"
        "int entry() { ^W[] ws = { W(7) }; return ws[9].dmg; }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 1);
        strataJitDestroy(jit);
    }
}

/* Moving an OOB element out must not poison later accesses of the same site
   (the null-the-source re-resolution must not free the moved dummy). */
STRATA_TEST(profile_oob_move_then_reread_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "extern ulong strlen(string s);\n"
        "int entry() { string[] s = {\"a\"}; string t = s[9]; string u = s[9]; return (int)(strlen(t) + strlen(u)); }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ(strataJitAddSymbol(jit, "strlen", (void*)&strlen), 1);
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 2);
        strataJitDestroy(jit);
    }
}

STRATA_TEST(profile_oob_box_move_then_reread_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "struct W { int dmg; };\n"
        "int entry() { ^W[] ws = { W(7) }; ^W m = ws[9]; return ws[9].dmg + m.dmg; }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 2);
        strataJitDestroy(jit);
    }
}

/* Repeated OOB events reuse the scratch cleanly (drop + fresh dummy). */
STRATA_TEST(profile_oob_repeat_in_loop_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit(
        "int entry() { int[] a = {1}; int total = 0; for (uint i = 0; i < 5; i++) { total += a[3]; a[4] = i; } return total; }",
        STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int violations = 0;
        STRATA_CHECK_EQ(CallEntryReport(jit, &violations), 0);
        STRATA_CHECK_EQ(violations, 10);
        strataJitDestroy(jit);
    }
}

STRATA_TEST(profile_null_extern_panics_when_unbound_llvm)
{
    /* Uses the non-unwinding recorder handler: the null-extern panic block
       RETURNS a zero value (rather than unreachable), so the handler may
       return and execution continues with a defined result. */
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit("extern int host_missing(int x);\n"
                                "int entry() { return host_missing(5); }",
                                STRATA_JIT_BACKEND_LLVM, 1);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return;
    }

    int violations = -1;
    int value = CallEntryReport(jit, &violations);

    STRATA_CHECK_EQ(violations, 1);
    STRATA_CHECK(strstr(s_panicMsg, "call to null extern function 'host_missing'") != NULL);
    STRATA_CHECK_EQ(value, 0); /* zero return after the panic */

    strataJitDestroy(jit);
}

STRATA_TEST(profile_checks_disabled_bounds_no_panic_llvm)
{
    if (!(strataCapabilities() & STRATA_CAP_LLVM_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit("int entry() { int[] a = {1, 2, 3}; return a[5]; }",
                                STRATA_JIT_BACKEND_LLVM, 0);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        ExpectNoPanic(jit);
        strataJitDestroy(jit);
    }
}

/* The emitted JIT C must contain the null-extern guard with the default
   profile and omit it when nullExternCall is disabled. */
