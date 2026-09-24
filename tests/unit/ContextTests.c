#include "Codegen/CodegenBackend.h"
#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

#include <stdint.h>
#include <string.h>

static StrataJit* CompileCtx(const char* src, const char** err)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(c, src, "ctx", err);
    strataCompilerDestroy(c);
    return jit;
}

/* The core regression this feature exists for: two independently-created
   contexts never clobber each other's globals. */
STRATA_TEST(context_two_instances_are_isolated)
{
    const char* err = NULL;
    StrataJit* jit = CompileCtx("int g_count = 0;\n"
                                "void bump() { g_count = g_count + 1; }\n"
                                "int get() { return g_count; }\n",
                                &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
    void (*bump)(void*) = (void (*)(void*))strataJitGetFunction(jit, "bump");
    int (*get)(void*) = (int (*)(void*))strataJitGetFunction(jit, "get");

    STRATA_CHECK(create != NULL && destroy != NULL && bump != NULL && get != NULL);
    if (create && destroy && bump && get)
    {
        void* ctxA = create();
        void* ctxB = create();
        STRATA_CHECK(ctxA != NULL && ctxB != NULL);
        STRATA_CHECK(ctxA != ctxB);

        bump(ctxA);
        bump(ctxA);
        bump(ctxA);
        bump(ctxB);

        STRATA_CHECK_EQ(get(ctxA), 3);
        STRATA_CHECK_EQ(get(ctxB), 1);

        destroy(ctxA);
        destroy(ctxB);
    }

    strataJitDestroy(jit);
}

/* A module with no storage-backed global exports no context symbols and
   every function keeps its exact, unchanged (no hidden param) signature. */
STRATA_TEST(context_absent_when_no_globals)
{
    const char* err = NULL;
    StrataJit* jit = CompileCtx("int add(int a, int b) { return a + b; }\n", &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    STRATA_CHECK(strataJitGetFunction(jit, "__strata_context_create") == NULL);
    STRATA_CHECK(strataJitGetFunction(jit, "__strata_context_destroy") == NULL);

    int (*add)(int, int) = (int (*)(int, int))strataJitGetFunction(jit, "add");
    STRATA_CHECK(add != NULL);
    if (add)
    {
        STRATA_CHECK_EQ(add(2, 3), 5);
    }

    strataJitDestroy(jit);
}

/* A module with only manifest-const globals (no runtime storage today) is
   likewise unaffected: still no context. */
STRATA_TEST(context_absent_for_manifest_const_only_globals)
{
    const char* err = NULL;
    StrataJit* jit = CompileCtx("const int cap = 7;\n"
                                "int entry() { return cap; }\n",
                                &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    STRATA_CHECK(strataJitGetFunction(jit, "__strata_context_create") == NULL);

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 7);
    }

    strataJitDestroy(jit);
}

/* AOT/IR shape: the context struct + create/destroy are always emitted as
   ordinary functions, never registered via llvm.global_ctors (unlike the
   old per-process __strata_module_init/teardown) — multiple independent
   instances can exist per process, so a host must always call them
   explicitly. */
STRATA_TEST(context_ir_has_no_global_ctors)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("string g = \"hi\";\n"
                                  "int entry() { return (int)g.length; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "__strata_context_create") != NULL);
    STRATA_CHECK(strstr(res.output, "__strata_context_destroy") != NULL);
    STRATA_CHECK(strstr(res.output, "llvm.global_ctors") == NULL);

    free((void*)res.output);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* Impl methods with an inline (non-extern) body also get the hidden
   pointer and correctly forward it on nested calls. */
STRATA_TEST(context_threads_through_inline_impl_methods)
{
    const char* err = NULL;
    StrataJit* jit = CompileCtx("handle Widget;\n"
                                "int g_total = 0;\n"
                                "impl Widget {\n"
                                "    void Add(Widget self, int n) { g_total = g_total + n; }\n"
                                "}\n"
                                "int run(Widget w) { w.Add(4); w.Add(5); return g_total; }\n",
                                &err);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
    int (*run)(void*, void*) = (int (*)(void*, void*))strataJitGetFunction(jit, "run");
    STRATA_CHECK(create != NULL && destroy != NULL && run != NULL);

    if (create && destroy && run)
    {
        void* ctx = create();
        STRATA_CHECK_EQ(run(ctx, (void*)(uintptr_t)0x1234 /* opaque handle value, unused by the script */), 9);
        destroy(ctx);
    }

    strataJitDestroy(jit);
}

/* The user-visible signature check ignores the hidden context parameter:
   with globals, `int main()` is still an int() entry (called as int(void*)),
   while `float main()` or `int main(int)` are not. */
STRATA_TEST(context_int_void_signature_ignores_hidden_param)
{
    const char* err = NULL;
    StrataJit* jit = CompileCtx("int g = 1;\n"
                                "int main() { return g; }\n"
                                "float fmain() { return 1.5; }\n"
                                "int withArg(int x) { return x + g; }\n",
                                &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        return;
    }

    STRATA_CHECK_EQ(strataJitHasContext(jit), 1);
    STRATA_CHECK_EQ(strataJitHasIntVoidSignature(jit, "main"), 1);
    STRATA_CHECK_EQ(strataJitHasIntVoidSignature(jit, "fmain"), 0);
    STRATA_CHECK_EQ(strataJitHasIntVoidSignature(jit, "withArg"), 0);
    STRATA_CHECK_EQ(strataJitHasIntVoidSignature(jit, "missing"), 0);
    /* Not callable as a bare int(void): it needs the context pointer. */
    STRATA_CHECK_EQ(strataJitCanInvokeIntVoid(jit, "main"), 0);

    void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "main");
    STRATA_CHECK(create && destroy && entry);
    if (create && destroy && entry)
    {
        void* ctx = create();
        STRATA_CHECK_EQ(entry(ctx), 1);
        destroy(ctx);
    }

    strataJitDestroy(jit);

    /* Without globals: no context, and int(void) is directly invokable. */
    jit = CompileCtx("int main() { return 4; }\nfloat fmain() { return 1.5; }\n", &err);
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ(strataJitHasContext(jit), 0);
        STRATA_CHECK_EQ(strataJitHasIntVoidSignature(jit, "main"), 1);
        STRATA_CHECK_EQ(strataJitCanInvokeIntVoid(jit, "main"), 1);
        STRATA_CHECK_EQ(strataJitCanInvokeIntVoid(jit, "fmain"), 0);
        strataJitDestroy(jit);
    }
}
