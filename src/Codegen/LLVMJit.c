#include "LLVMJit.h"
#include "Codegen/LLVMCApi.h"
#include "LLVMTargets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strata heap runtime backing for the LLVM JIT (host malloc/free). Generated
   code calls strata_alloc(i64): the size parameter must be 64-bit on every
   target (`unsigned long` is 32-bit on Win64). */
static void* strata_alloc_impl(uint64_t n)
{
    return malloc((size_t)n);
}

static void strata_free_impl(void* p)
{
    free(p);
}

extern void strata_panic(const char* msg);
extern void strata_oob(const char* msg);

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

/* Formats an LLVMErrorRef into a malloc'd errorMessage (caller frees), then
   disposes the error. */
static void SetOrcError(char** errorMessage, LLVMErrorRef err, const char* fmt)
{
    char* msg = TakeOrcError(err);
    int needed = snprintf(NULL, 0, fmt, msg);
    char* buf = (char*)malloc((size_t)needed + 1);
    snprintf(buf, (size_t)needed + 1, fmt, msg);
    *errorMessage = buf;
    free(msg);
}

#ifdef _WIN32
/* Overrides LLJIT's default object layer. The legacy RuntimeDyld/COFF stack
   allocates each section with its own VirtualAlloc, and Win64 .pdata/.xdata
   unwind info carries IMAGE_REL_AMD64_ADDR32NB relocations that fatal-error
   whenever a module's sections land more than 2GB apart — intermittent in
   hosts that allocate gigabytes (llvm/llvm-project#65641). The ReserveAlloc
   section memory manager carves all sections out of one reserved region. */
static LLVMOrcObjectLayerRef ReserveAllocObjectLayerCreator(void* ctx, LLVMOrcExecutionSessionRef es,
                                                            const char* triple)
{
    (void)ctx;
    (void)triple;

    return LLVMOrcCreateRTDyldObjectLinkingLayerWithSectionMemoryManagerReserveAlloc(es, 1);
}
#endif

/* Disposes the module/context held by a BuiltModule after a failed load. */
static void DisposeBuiltModuleOnError(BuiltModule* bm)
{
    LLVMDisposeModule(bm->mod);
    LLVMContextDispose(bm->ctx);
    bm->mod = NULL;
    bm->ctx = NULL;
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

/* The triple the JIT compiles for, valid for the life of the process. On x86-64 Windows that is the host's with ELF
   objects instead of COFF: COFF puts every floating-point and vector constant in a COMDAT section (`__real@...`,
   `__xmm@...`) that the reserve-allocating RuntimeDyld layer intermittently fails to load ("Failed to
   materialize symbols"), where ELF objects keep constants in plain read-only sections. The calling convention
   stays Win64. STRATA_JIT_COFF=1 keeps the host's own object format. */
static const char* JitTriple(void)
{
    static char* hostTriple = NULL;

    if (!hostTriple)
    {
        hostTriple = LLVMGetDefaultTargetTriple();
    }

#ifdef _WIN32
    if (hostTriple && strncmp(hostTriple, "x86_64-pc-windows", 17) == 0 && !getenv("STRATA_JIT_COFF"))
    {
        return "x86_64-pc-windows-elf";
    }
#endif

    return hostTriple;
}

/* Creates an execution TargetMachine for the host CPU: every feature the
   processor supports enabled, aggressive codegen level, PIC. The module's
   triple/data layout should be pinned to this machine before IR passes run.
   Returns NULL and sets *errorMessage (malloc-owned) on failure. */
static LLVMTargetMachineRef CreateHostTargetMachine(char** errorMessage)
{
    const char* triple = JitTriple();
    LLVMTargetRef target = NULL;
    char* targetErr = NULL;

    if (LLVMGetTargetFromTriple(triple, &target, &targetErr))
    {
        int needed = snprintf(NULL, 0, "could not resolve target '%s': %s", triple ? triple : "(null)",
                              targetErr ? targetErr : "(no message)");
        char* buf = (char*)malloc((size_t)needed + 1);
        snprintf(buf, (size_t)needed + 1, "could not resolve target '%s': %s", triple ? triple : "(null)",
                 targetErr ? targetErr : "(no message)");
        *errorMessage = buf;

        if (targetErr)
        {
            /* A plain LLVM message (not from LLVMGetErrorMessage). */
            LLVMDisposeMessage(targetErr);
        }

        return NULL;
    }

    char* cpu = LLVMGetHostCPUName();
    char* features = LLVMGetHostCPUFeatures();

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, cpu, features, LLVMCodeGenLevelAggressive,
                                                      LLVMRelocPIC, LLVMCodeModelJITDefault);

    LLVMDisposeMessage(features);
    LLVMDisposeMessage(cpu);

    if (!tm)
    {
        *errorMessage = DupString("could not create target machine for host");
    }

    return tm;
}

void LLVMJitInit(LLVMJit* jit)
{
    jit->jit = NULL;
    jit->allocFn = NULL;
    jit->freeFn = NULL;
    VecInit(&jit->externs);
}

void LLVMJitSetAllocFree(LLVMJit* jit, void* allocFn, void* freeFn)
{
    jit->allocFn = allocFn;
    jit->freeFn = freeFn;
}

void LLVMJitDestroy(LLVMJit* jit)
{
    if (jit->jit)
    {
        /* Unlike the old per-process __strata_module_teardown, a module with
           instanced globals is never auto-torn-down here: the host may have
           created zero, one, or many __strata_context_create instances, and
           owns calling __strata_context_destroy on each (discoverable via
           strataJitGetFunction like any other exported symbol). */
        LLVMErrorRef disposeErr = LLVMOrcDisposeLLJIT(jit->jit);

        if (disposeErr)
        {
            LLVMConsumeError(disposeErr);
        }

        jit->jit = NULL;
    }

    for (size_t i = 0; i < jit->externs.count; i++)
    {
        char* sym = (char*)jit->externs.items[i];
        free(sym);
    }

    free(jit->externs.items);
    VecInit(&jit->externs);
}

static void JitCopyExternSymbols(LLVMJit* jit, BuiltModule* bm)
{
    for (size_t i = 0; i < bm->externSymbols.count; i++)
    {
        const char* sym = (const char*)VecGet(&bm->externSymbols, i);
        VecPush(&jit->externs, DupString(sym));
    }
}

static LLVMTargetMachineRef JitCreateTargetMachine(BuiltModule* bm, char** errorMessage)
{
    char* tmError = NULL;
    LLVMTargetMachineRef tm = CreateHostTargetMachine(&tmError);

    if (!tm)
    {
        *errorMessage = tmError ? tmError : DupString("could not create execution engine");

        DisposeBuiltModuleOnError(bm);

        return NULL;
    }

    return tm;
}

static void JitPinTargetLayout(BuiltModule* bm, LLVMTargetMachineRef tm)
{
    const char* triple = JitTriple();

    LLVMSetTarget(bm->mod, triple);

    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);

    LLVMSetModuleDataLayout(bm->mod, td);
    LLVMDisposeTargetData(td);
}

static bool JitRunOptimizationPipeline(BuiltModule* bm, LLVMTargetMachineRef tm, char** errorMessage)
{
    /* STRATA_OPT_PIPELINE overrides the pass pipeline (e.g. "default<O2>")
       for experimentation; "default<O3>" is the shipped default. */
    const char* pipeline = getenv("STRATA_OPT_PIPELINE");
    if (!pipeline || !pipeline[0])
    {
        pipeline = "default<O3>";
    }

    LLVMPassBuilderOptionsRef pbOptions = LLVMCreatePassBuilderOptions();

    LLVMPassBuilderOptionsSetLoopVectorization(pbOptions, 1);
    LLVMPassBuilderOptionsSetSLPVectorization(pbOptions, 1);

    LLVMErrorRef passErr = LLVMRunPasses(bm->mod, pipeline, tm, pbOptions);

    LLVMDisposePassBuilderOptions(pbOptions);

    if (passErr)
    {
        SetOrcError(errorMessage, passErr, "could not optimize module: %s");

        LLVMDisposeTargetMachine(tm);

        DisposeBuiltModuleOnError(bm);

        return false;
    }

    return true;
}

static LLVMOrcLLJITRef JitCreateLLJIT(LLVMTargetMachineRef tm, BuiltModule* bm, char** errorMessage)
{
    /* Transfers ownership of tm to the builder. */
    LLVMOrcJITTargetMachineBuilderRef jtmb = LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(tm);

    LLVMOrcLLJITBuilderRef builder = LLVMOrcCreateLLJITBuilder();
    LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(builder, jtmb);

#ifdef _WIN32
    LLVMOrcLLJITBuilderSetObjectLinkingLayerCreator(builder, &ReserveAllocObjectLayerCreator, NULL);
#endif

    LLVMOrcLLJITRef llj = NULL;
    LLVMErrorRef err = LLVMOrcCreateLLJIT(&llj, builder);

    if (err)
    {
        SetOrcError(errorMessage, err, "could not create execution engine: %s");

        DisposeBuiltModuleOnError(bm);

        return NULL;
    }

    return llj;
}

/* Reflect process symbols (e.g. libcalls like memcpy/memset that LLVM
   may lower struct-copy or vector code into) into the main dylib, since
   a bare JITDylib does not resolve these automatically. */
static void JitAddProcessSymbolGenerator(LLVMOrcLLJITRef llj, LLVMOrcJITDylibRef mainJd)
{
    LLVMOrcDefinitionGeneratorRef gen = NULL;
    LLVMErrorRef genErr
        = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&gen, LLVMOrcLLJITGetGlobalPrefix(llj), NULL, NULL);

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
static void JitDefineHeapSymbols(LLVMJit* jit, LLVMOrcLLJITRef llj, LLVMOrcJITDylibRef mainJd)
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
        /* On failure the caller keeps ownership of the unit. */
        LLVMConsumeError(defineErr);
        LLVMOrcDisposeMaterializationUnit(mu);
    }
}

static bool JitAddModule(LLVMJit* jit, LLVMOrcLLJITRef llj, LLVMOrcJITDylibRef mainJd, BuiltModule* bm,
                         char** errorMessage)
{
    LLVMOrcThreadSafeContextRef tsCtx = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(bm->ctx);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(bm->mod, tsCtx);
    LLVMOrcDisposeThreadSafeContext(tsCtx);
    bm->ctx = NULL;
    bm->mod = NULL;

    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(llj, mainJd, tsm);

    if (err)
    {
        SetOrcError(errorMessage, err, "could not add module to execution engine: %s");

        /* LLVMOrcLLJITAddLLVMIRModule takes ownership of tsm even on
           failure, so it must not be disposed again here. */

        LLVMErrorRef disposeErr = LLVMOrcDisposeLLJIT(llj);

        if (disposeErr)
        {
            LLVMConsumeError(disposeErr);
        }

        jit->jit = NULL;

        return false;
    }

    /* Every module now exports __strata_context_create
       (see LLVMModuleBuilder.c) instead of auto-running a single per-process
       __strata_module_init: the host calls it explicitly, as many times as
       it wants independent instances, exactly like any other symbol. */

    return true;
}

bool LLVMJitLoad(LLVMJit* jit, BuiltModule* bm, char** errorMessage)
{
    EnsureTargetsInitialized();

    if (!bm->mod)
    {
        *errorMessage = DupString("no module to JIT");

        return false;
    }

    JitCopyExternSymbols(jit, bm);

    /* Build an execution TargetMachine for the host CPU (all features,
       aggressive codegen level), pin the module's triple/data layout to it,
       and run the full IR optimization pipeline before handing the module to
       ORC. Without this the JIT materialized codegen output directly: no
       mem2reg/SROA, no inliner, no GVN, no vectorization. */
    LLVMTargetMachineRef tm = JitCreateTargetMachine(bm, errorMessage);

    if (!tm)
    {
        return false;
    }

    JitPinTargetLayout(bm, tm);

    if (!JitRunOptimizationPipeline(bm, tm, errorMessage))
    {
        return false;
    }

    LLVMOrcLLJITRef llj = JitCreateLLJIT(tm, bm, errorMessage);

    if (!llj)
    {
        return false;
    }

    jit->jit = llj;

    LLVMOrcJITDylibRef mainJd = LLVMOrcLLJITGetMainJITDylib(llj);

    JitAddProcessSymbolGenerator(llj, mainJd);
    JitDefineHeapSymbols(jit, llj, mainJd);

    return JitAddModule(jit, llj, mainJd, bm, errorMessage);
}

bool LLVMJitAddSymbol(LLVMJit* jit, const char* name, void* addr)
{
    if (!jit->jit || !name || !addr)
    {
        return false;
    }

    char slot[256];
    snprintf(slot, sizeof(slot), "__strata_ext_%s", name);

    uint64_t slotAddr = 0;

    if (!OrcLookup(jit->jit, slot, &slotAddr) || slotAddr == 0)
    {
        return false;
    }

    *(void**)slotAddr = addr;

    return true;
}

uint64_t LLVMJitGetAddress(LLVMJit* jit, const char* name)
{
    if (!jit->jit || !name)
    {
        return 0;
    }

    uint64_t addr = 0;

    if (!OrcLookup(jit->jit, name, &addr))
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

    return jit->externs.count;
}

const char* LLVMJitExternName(const LLVMJit* jit, size_t index)
{
    if (!jit || index >= jit->externs.count)
    {
        return NULL;
    }

    return (const char*)jit->externs.items[index];
}
