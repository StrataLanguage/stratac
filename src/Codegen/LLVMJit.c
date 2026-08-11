#include "LLVMJit.h"
#include "Codegen/LLVMCApi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strata heap runtime backing for the LLVM JIT (host malloc/free). */
static void* strata_alloc_impl(unsigned long n)
{
    return malloc((size_t)n);
}

static void strata_free_impl(void* p)
{
    free(p);
}

extern void strata_panic(const char* msg);

static void EnsureTargetsInitialized(void)
{
    static bool initialized = false;

    if (!initialized)
    {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();

        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();

        initialized = true;
    }
}

void LLVMJitInit(LLVMJit* jit)
{
    jit->m_ee = NULL;
    jit->m_ctx = NULL;
    jit->m_mod = NULL;
    VecInit(&jit->m_externs);
}

void LLVMJitDestroy(LLVMJit* jit)
{
    if (jit->m_ee)
    {
        /* If the module has owning globals, their teardown was emitted as
           __strata_module_teardown.  Call it before we unload. */
        typedef void (*VoidFn)(void);
        VoidFn td = (VoidFn)(uintptr_t)LLVMGetFunctionAddress(jit->m_ee, "__strata_module_teardown");

        if (td)
        {
            td();
        }
    }

    if (jit->m_ee)
    {
        LLVMDisposeExecutionEngine(jit->m_ee);
        jit->m_ee = NULL;
    }

    if (jit->m_ctx)
    {
        LLVMContextDispose(jit->m_ctx);
        jit->m_ctx = NULL;
    }

    for (size_t i = 0; i < jit->m_externs.count; i++)
    {
        char* sym = (char*)jit->m_externs.items[i];
        free(sym);
    }

    free(jit->m_externs.items);
    VecInit(&jit->m_externs);

    jit->m_mod = NULL;
}

bool LLVMJitLoad(LLVMJit* jit, BuiltModule* bm, char** errorMessage)
{
    EnsureTargetsInitialized();

    if (!bm->mod)
    {
        *errorMessage = DupString("no module to JIT");

        return false;
    }

    for (size_t i = 0; i < bm->externSymbols.count; i++)
    {
        const char* sym = (const char*)VecGet(&bm->externSymbols, i);
        VecPush(&jit->m_externs, DupString(sym));
    }

    LLVMContextRef context = bm->ctx;
    LLVMModuleRef modRef = bm->mod;
    bm->ctx = NULL;
    bm->mod = NULL;

    jit->m_ctx = context;

    char* error = NULL;

    if (LLVMCreateExecutionEngineForModule(&jit->m_ee, modRef, &error))
    {
        const char* msg = error ? error : "(no message)";
        int needed = snprintf(NULL, 0, "could not create execution engine: %s", msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "could not create execution engine: %s", msg);
        *errorMessage = buf;

        if (error)
        {
            LLVMDisposeMessage(error);
        }

        LLVMDisposeModule(modRef);
        jit->m_ee = NULL;
        LLVMContextDispose(jit->m_ctx);
        jit->m_ctx = NULL;

        return false;
    }

    jit->m_mod = modRef;

    /* Map the compiler-internal heap runtime to the host so generated box
       code can allocate without the user binding symbols. */
    {
        LLVMValueRef allocFn = LLVMGetNamedFunction(modRef, "strata_alloc");

        if (allocFn)
        {
            LLVMAddGlobalMapping(jit->m_ee, allocFn, (void*)&strata_alloc_impl);
        }

        LLVMValueRef freeFn = LLVMGetNamedFunction(modRef, "strata_free");

        if (freeFn)
        {
            LLVMAddGlobalMapping(jit->m_ee, freeFn, (void*)&strata_free_impl);
        }

        LLVMValueRef panicFn = LLVMGetNamedFunction(modRef, "strata_panic");

        if (panicFn)
        {
            LLVMAddGlobalMapping(jit->m_ee, panicFn, (void*)&strata_panic);
        }
    }

    /* If the module has owning globals (box<T> / T[]), their runtime
       initialisation was emitted as __strata_module_init.  Call it now. */
    {
        typedef void (*VoidFn)(void);
        VoidFn init = (VoidFn)(uintptr_t)LLVMGetFunctionAddress(jit->m_ee, "__strata_module_init");

        if (init)
        {
            init();
        }
    }

    return true;
}

bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr)
{
    if (!jit->m_ee || !name || !addr)
    {
        return false;
    }

    char slot[256];
    snprintf(slot, sizeof(slot), "__strata_ext_%s", name);
    uint64_t slotAddr = LLVMGetGlobalValueAddress(jit->m_ee, slot);

    if (slotAddr == 0)
    {
        return false;
    }

    *(void**)slotAddr = addr;

    return true;
}

uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name)
{
    if (!jit->m_ee || !name)
    {
        return 0;
    }

    return (uint64_t)LLVMGetFunctionAddress(jit->m_ee, name);
}
