#include "strata/strata.h"

#include "Codegen/CBackend.h"
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

static void RunPanicPair(StrataJitBackend backend, const char* source, const char* expectMsg)
{
    unsigned caps = strataCapabilities();

    if (backend == STRATA_JIT_BACKEND_TCC && !(caps & STRATA_CAP_TCC_JIT))
    {
        return;
    }

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

STRATA_TEST(profile_bounds_check_panics_on_oob_tcc)
{
    RunPanicPair(STRATA_JIT_BACKEND_TCC,
                 "int entry() { int[] a = {1, 2, 3}; return a[5]; }",
                 "array index out of bounds");
}

/* An errored module must be rejected cleanly (NULL jit + message), never
   crash the compiler — a return that reads out a NULL box-inner used to
   pass NULL into TypeRegistryIsOwningStruct and crash mid-resolve. */
static void CheckErrorRejected(StrataJitBackend backend, const char* source, const char* expectMsg)
{
    unsigned caps = strataCapabilities();

    if (backend == STRATA_JIT_BACKEND_TCC && !(caps & STRATA_CAP_TCC_JIT))
    {
        return;
    }

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

STRATA_TEST(errored_module_rejected_cleanly_tcc)
{
    CheckErrorRejected(STRATA_JIT_BACKEND_TCC,
                       "box<string>[] g = { \"Hi\" };\n"
                       "string[] f() { return g; }\n",
                       "cannot return a value of type 'box<string>[]'");
}

STRATA_TEST(errored_module_rejected_cleanly_llvm)
{
    CheckErrorRejected(STRATA_JIT_BACKEND_LLVM,
                       "box<string>[] g = { \"Hi\" };\n"
                       "string[] f() { return g; }\n",
                       "cannot return a value of type 'box<string>[]'");
}

STRATA_TEST(errored_module_rejected_cleanly_string_mismatch)
{
    CheckErrorRejected(STRATA_JIT_BACKEND_TCC,
                       "int f() { string s = \"x\"; return s; }\n",
                       "cannot return a value of type 'string'");
}

STRATA_TEST(profile_bounds_check_panics_on_oob_llvm)
{
    RunPanicPair(STRATA_JIT_BACKEND_LLVM,
                 "int entry() { int[] a = {1, 2, 3}; return a[5]; }",
                 "array index out of bounds");
}

STRATA_TEST(profile_null_extern_panics_when_unbound_tcc)
{
    RunPanicPair(STRATA_JIT_BACKEND_TCC,
                 "extern int host_missing(int x);\n"
                 "int entry() { return host_missing(5); }",
                 "call to null extern function 'host_missing'");
}

STRATA_TEST(profile_null_extern_panics_when_unbound_llvm)
{
    RunPanicPair(STRATA_JIT_BACKEND_LLVM,
                 "extern int host_missing(int x);\n"
                 "int entry() { return host_missing(5); }",
                 "call to null extern function 'host_missing'");
}

STRATA_TEST(profile_checks_disabled_bounds_no_panic_tcc)
{
    if (!(strataCapabilities() & STRATA_CAP_TCC_JIT))
    {
        return;
    }

    StrataJit* jit = CompileJit("int entry() { int[] a = {1, 2, 3}; return a[5]; }",
                                STRATA_JIT_BACKEND_TCC, 0);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        ExpectNoPanic(jit);
        strataJitDestroy(jit);
    }
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
STRATA_TEST(profile_null_extern_guard_reflects_profile_in_c)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    const char* src = "extern int host_missing(int x); int entry() { return host_missing(5); }";
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StrataProfile off = strataProfileDefault();
    off.nullExternCall = 0;

    BuiltCModule withDefault = BuildCModule(mod, &diag, &arena, CEmitJIT, STRATA_ARCH_AUTO, NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(withDefault.source, "strata__ext_check") != NULL);
    STRATA_CHECK(strstr(withDefault.source, "call to null extern function 'host_missing'") != NULL);

    BuiltCModule withOff = BuildCModule(mod, &diag, &arena, CEmitJIT, STRATA_ARCH_AUTO, &off);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(withOff.source, "strata__ext_check") == NULL);

    BuiltCModuleDispose(&withDefault);
    BuiltCModuleDispose(&withOff);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* The emitted JIT C must contain the bounds check with the default profile
   and omit it when boundsCheck is disabled. */
STRATA_TEST(profile_bounds_check_reflects_profile_in_c)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);

    const char* src = "int entry() { int[] a = {1, 2, 3}; return a[0]; }";
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StrataProfile off = strataProfileDefault();
    off.boundsCheck = 0;

    BuiltCModule withDefault = BuildCModule(mod, &diag, &arena, CEmitJIT, STRATA_ARCH_AUTO, NULL);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(withDefault.source, "array index out of bounds") != NULL);

    BuiltCModule withOff = BuildCModule(mod, &diag, &arena, CEmitJIT, STRATA_ARCH_AUTO, &off);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(strstr(withOff.source, "array index out of bounds") == NULL);

    BuiltCModuleDispose(&withDefault);
    BuiltCModuleDispose(&withOff);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
