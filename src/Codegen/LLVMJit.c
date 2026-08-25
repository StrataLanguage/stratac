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
extern void strata_oob(const char* msg);

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

/* Converts an LLVMErrorRef into a malloc-owned string (freed by the caller
   with free(), matching this file's errorMessage out-param convention),
   consuming/disposing the LLVM-owned message. Returns NULL for a null/success
   error. */
static char* TakeOrcError(LLVMErrorRef err)
{
    if (!err)
    {
        return NULL;
    }

    char* llvmMsg = LLVMGetErrorMessage(err);
    char* copy = DupString(llvmMsg ? llvmMsg : "(no message)");

    if (llvmMsg)
    {
        LLVMDisposeErrorMessage(llvmMsg);
    }

    return copy;
}

static bool OrcLookup(LLVMOrcLLJITRef jit, const char* name, uint64_t* outAddr)
{
    LLVMOrcExecutorAddress addr = 0;
    LLVMErrorRef err = LLVMOrcLLJITLookup(jit, &addr, name);

    if (err)
    {
        LLVMConsumeError(err);

        return false;
    }

    *outAddr = (uint64_t)addr;

    return true;
}

void LLVMJitInit(LLVMJit* jit)
{
    jit->m_jit = NULL;
    jit->allocFn = NULL;
    jit->freeFn = NULL;
    VecInit(&jit->m_externs);
}

void LLVMJitSetAllocFree(LLVMJit* jit, void* allocFn, void* freeFn)
{
    jit->allocFn = allocFn;
    jit->freeFn = freeFn;
}

void LLVMJitDestroy(LLVMJit* jit)
{
    if (jit->m_jit)
    {
        /* If the module has owning globals, their teardown was emitted as
           __strata_module_teardown.  Call it before we unload. */
        typedef void (*VoidFn)(void);
        uint64_t tdAddr = 0;

        if (OrcLookup(jit->m_jit, "__strata_module_teardown", &tdAddr) && tdAddr)
        {
            ((VoidFn)(uintptr_t)tdAddr)();
        }

        LLVMErrorRef disposeErr = LLVMOrcDisposeLLJIT(jit->m_jit);

        if (disposeErr)
        {
            LLVMConsumeError(disposeErr);
        }

        jit->m_jit = NULL;
    }

    for (size_t i = 0; i < jit->m_externs.count; i++)
    {
        char* sym = (char*)jit->m_externs.items[i];
        free(sym);
    }

    free(jit->m_externs.items);
    VecInit(&jit->m_externs);
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

    LLVMOrcJITTargetMachineBuilderRef jtmb = NULL;
    LLVMErrorRef err = LLVMOrcJITTargetMachineBuilderDetectHost(&jtmb);

    if (err)
    {
        char* msg = TakeOrcError(err);
        int needed = snprintf(NULL, 0, "could not create execution engine: %s", msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "could not create execution engine: %s", msg);
        *errorMessage = buf;
        free(msg);

        LLVMDisposeModule(bm->mod);
        LLVMContextDispose(bm->ctx);
        bm->mod = NULL;
        bm->ctx = NULL;

        return false;
    }

    LLVMOrcLLJITBuilderRef builder = LLVMOrcCreateLLJITBuilder();
    LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(builder, jtmb);

    LLVMOrcLLJITRef llj = NULL;
    err = LLVMOrcCreateLLJIT(&llj, builder);

    if (err)
    {
        char* msg = TakeOrcError(err);
        int needed = snprintf(NULL, 0, "could not create execution engine: %s", msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "could not create execution engine: %s", msg);
        *errorMessage = buf;
        free(msg);

        LLVMDisposeModule(bm->mod);
        LLVMContextDispose(bm->ctx);
        bm->mod = NULL;
        bm->ctx = NULL;

        return false;
    }

    jit->m_jit = llj;

    LLVMOrcJITDylibRef mainJd = LLVMOrcLLJITGetMainJITDylib(llj);

    /* Reflect process symbols (e.g. libcalls like memcpy/memset that LLVM
       may lower struct-copy or vector code into) into the main dylib, since
       a bare JITDylib does not resolve these automatically. */
    {
        LLVMOrcDefinitionGeneratorRef gen = NULL;
        LLVMErrorRef genErr = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
            &gen, LLVMOrcLLJITGetGlobalPrefix(llj), NULL, NULL);

        if (!genErr)
        {
            LLVMOrcJITDylibAddGenerator(mainJd, gen);
        }
        else
        {
            LLVMConsumeError(genErr);
        }
    }

    /* Map the compiler-internal heap runtime to the host so generated box
       code can allocate without the user binding symbols. */
    {
        LLVMOrcCSymbolMapPair pairs[4];

        pairs[0].Name = LLVMOrcLLJITMangleAndIntern(llj, "strata_alloc");
        pairs[0].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)(jit->allocFn ? jit->allocFn : (void*)&strata_alloc_impl);
        pairs[0].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
        pairs[0].Sym.Flags.TargetFlags = 0;

        pairs[1].Name = LLVMOrcLLJITMangleAndIntern(llj, "strata_free");
        pairs[1].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)(jit->freeFn ? jit->freeFn : (void*)&strata_free_impl);
        pairs[1].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
        pairs[1].Sym.Flags.TargetFlags = 0;

        pairs[2].Name = LLVMOrcLLJITMangleAndIntern(llj, "strata_panic");
        pairs[2].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)(void*)&strata_panic;
        pairs[2].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
        pairs[2].Sym.Flags.TargetFlags = 0;

        pairs[3].Name = LLVMOrcLLJITMangleAndIntern(llj, "strata_oob");
        pairs[3].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)(void*)&strata_oob;
        pairs[3].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
        pairs[3].Sym.Flags.TargetFlags = 0;

        LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs, 4);
        LLVMErrorRef defineErr = LLVMOrcJITDylibDefine(mainJd, mu);

        if (defineErr)
        {
            LLVMConsumeError(defineErr);
        }
    }

    LLVMOrcThreadSafeContextRef tsCtx = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(bm->ctx);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(bm->mod, tsCtx);
    LLVMOrcDisposeThreadSafeContext(tsCtx);
    bm->ctx = NULL;
    bm->mod = NULL;

    err = LLVMOrcLLJITAddLLVMIRModule(llj, mainJd, tsm);

    if (err)
    {
        char* msg = TakeOrcError(err);
        int needed = snprintf(NULL, 0, "could not add module to execution engine: %s", msg);
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "could not add module to execution engine: %s", msg);
        *errorMessage = buf;
        free(msg);

        LLVMOrcDisposeThreadSafeModule(tsm);

        LLVMErrorRef disposeErr = LLVMOrcDisposeLLJIT(llj);

        if (disposeErr)
        {
            LLVMConsumeError(disposeErr);
        }

        jit->m_jit = NULL;

        return false;
    }

    /* If the module has owning globals (box<T> / T[]), their runtime
       initialisation was emitted as __strata_module_init.  Call it now. */
    {
        typedef void (*VoidFn)(void);
        uint64_t initAddr = 0;

        if (OrcLookup(llj, "__strata_module_init", &initAddr) && initAddr)
        {
            ((VoidFn)(uintptr_t)initAddr)();
        }
    }

    return true;
}

bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr)
{
    if (!jit->m_jit || !name || !addr)
    {
        return false;
    }

    char slot[256];
    snprintf(slot, sizeof(slot), "__strata_ext_%s", name);

    uint64_t slotAddr = 0;

    if (!OrcLookup(jit->m_jit, slot, &slotAddr) || slotAddr == 0)
    {
        return false;
    }

    *(void**)slotAddr = addr;

    return true;
}

uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name)
{
    if (!jit->m_jit || !name)
    {
        return 0;
    }

    uint64_t addr = 0;

    if (!OrcLookup(jit->m_jit, name, &addr))
    {
        return 0;
    }

    return addr;
}

size_t LLVMJitExternCount(const LLVMJit* jit)
{
    if (!jit)
    {
        return 0;
    }

    return jit->m_externs.count;
}

const char* LLVMJitExternName(const LLVMJit* jit, size_t index)
{
    if (!jit || index >= jit->m_externs.count)
    {
        return NULL;
    }

    return (const char*)jit->m_externs.items[index];
}
