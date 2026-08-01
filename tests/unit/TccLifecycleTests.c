#include "Codegen/CBackend.h"
#include "Codegen/TccJit.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int CompileAndRunOnce(int value)
{
    char source[128];
    snprintf(source, sizeof(source), "int entry() { return %d; }", value);
    StrataCompiler* compiler = strataCompilerCreate();
    const char* error = NULL;
    StrataJit* jit = strataJitCompileString(compiler, source, "lifecycle", &error);
    int result = -1;
    if (jit)
    {
        int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        if (entry) result = entry();
        strataJitDestroy(jit);
    }
    strataFree((char*)error);
    strataCompilerDestroy(compiler);
    return result;
}

STRATA_TEST(tcc_jit_repeated_create_run_destroy)
{
    for (int i = 0; i < 50; ++i)
    {
        STRATA_CHECK_EQ(CompileAndRunOnce(i), i);
    }
}

STRATA_TEST(tcc_jit_reports_invalid_generated_c)
{
    BuiltCModule module;
    BuiltCModuleInit(&module);
    module.source = "int broken( {";
    TccJit jit;
    TccJitInit(&jit);
    char* error = NULL;
    STRATA_CHECK(!TccJitLoad(&jit, &module, &error));
    STRATA_CHECK(error != NULL);
    STRATA_CHECK(error && error[0] != '\0');
    free(error);
    TccJitDestroy(&jit);
    BuiltCModuleDispose(&module);
}

typedef struct {
    int base;
    int failed;
} ThreadContext;

#ifdef _WIN32
static DWORD WINAPI CompileThread(LPVOID opaque)
#else
static void* CompileThread(void* opaque)
#endif
{
    ThreadContext* context = (ThreadContext*)opaque;
    for (int i = 0; i < 20; ++i)
    {
        int expected = context->base + i;
        if (CompileAndRunOnce(expected) != expected)
        {
            context->failed = 1;
            break;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

STRATA_TEST(tcc_jit_parallel_compile_smoke)
{
    enum { ThreadCount = 4 };
    ThreadContext contexts[ThreadCount] = {0};
#ifdef _WIN32
    HANDLE threads[ThreadCount];
    for (int i = 0; i < ThreadCount; ++i)
    {
        contexts[i].base = i * 100;
        threads[i] = CreateThread(NULL, 0, CompileThread, &contexts[i], 0, NULL);
        STRATA_CHECK(threads[i] != NULL);
    }
    WaitForMultipleObjects(ThreadCount, threads, TRUE, INFINITE);
    for (int i = 0; i < ThreadCount; ++i)
    {
        if (threads[i]) CloseHandle(threads[i]);
        STRATA_CHECK(!contexts[i].failed);
    }
#else
    pthread_t threads[ThreadCount];
    for (int i = 0; i < ThreadCount; ++i)
    {
        contexts[i].base = i * 100;
        STRATA_CHECK_EQ(pthread_create(&threads[i], NULL, CompileThread, &contexts[i]), 0);
    }
    for (int i = 0; i < ThreadCount; ++i)
    {
        STRATA_CHECK_EQ(pthread_join(threads[i], NULL), 0);
        STRATA_CHECK(!contexts[i].failed);
    }
#endif
}
