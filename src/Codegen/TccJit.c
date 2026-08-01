#include "Codegen/TccJit.h"

#include "Core/Util.h"
#include "libtcc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void AppendDiagnostic(TccJit* jit, const char* message)
{
    size_t length = strlen(message);
    size_t needed = jit->diagnosticsLen + length + 2;

    if (needed > jit->diagnosticsCap)
    {
        size_t capacity = jit->diagnosticsCap ? jit->diagnosticsCap * 2 : 256;
        while (capacity < needed)
        {
            capacity *= 2;
        }

        char* diagnostics = (char*)realloc(jit->diagnostics, capacity);
        if (!diagnostics)
        {
            STRATA_OOM();
        }

        jit->diagnostics = diagnostics;
        jit->diagnosticsCap = capacity;
    }

    memcpy(jit->diagnostics + jit->diagnosticsLen, message, length);
    jit->diagnosticsLen += length;
    jit->diagnostics[jit->diagnosticsLen++] = '\n';
    jit->diagnostics[jit->diagnosticsLen] = '\0';
}

static void TccError(void* opaque, const char* message)
{
    AppendDiagnostic((TccJit*)opaque, message);
}

/* Strata heap runtime: backed by the host's malloc/free. */
static void* strata_alloc_impl(unsigned long n)
{
    return malloc((size_t)n);
}

static void strata_free_impl(void* p)
{
    free(p);
}

static void CopySymbols(TccJit* jit, Vec* destination, const Vec* source)
{
    for (size_t i = 0; i < source->count; ++i)
    {
        const CBackendSymbol* input = (const CBackendSymbol*)VecGet(source, i);

        TccJitSymbol* output = (TccJitSymbol*)arena_alloc(jit->tccArena, sizeof(TccJitSymbol));
        if (!output)
        {
            STRATA_CRASH("WTF");
        }

        output->strataName = arena_strdup(jit->tccArena, input->strataName);
        output->cName = arena_strdup(jit->tccArena, input->cName);
        output->isIntVoid = input->isIntVoid;
        VecPush(destination, output);
    }
}

static void FreeSymbols(Vec* symbols)
{
    // symbol data itself is allocated via arena.

    free(symbols->items);

    VecInit(symbols);
}

static TccJitSymbol* FindSymbol(const Vec* symbols, const char* name)
{
    for (size_t i = 0; i < symbols->count; ++i)
    {
        TccJitSymbol* symbol = (TccJitSymbol*)VecGet(symbols, i);

        if (strcmp(symbol->strataName, name) == 0)
        {
            return symbol;
        }
    }

    return NULL;
}

void TccJitInit(TccJit* jit)
{
    *jit = (TccJit){0};
    VecInit(&jit->exports);
    VecInit(&jit->externs);

    jit->tccArena = (Arena*)malloc(sizeof(Arena));
    if (!jit->tccArena)
    {
        STRATA_OOM();
    }

    arena_init(jit->tccArena, 0);
}

void TccJitDestroy(TccJit* jit)
{
    if (jit->state)
    {
        if (jit->relocated)
        {
            void (*moduleTeardown)(void) = (void (*)(void))tcc_get_symbol(jit->state, "__strata_module_teardown");

            if (moduleTeardown)
            {
                moduleTeardown();
            }
        }

        tcc_delete(jit->state);

        jit->state = NULL;
        jit->relocated = false;
    }
    
    FreeSymbols(&jit->exports);
    FreeSymbols(&jit->externs);
    
    free(jit->diagnostics);

    arena_free(jit->tccArena);
    free(jit->tccArena);
    jit->tccArena = NULL;

    jit->diagnostics = NULL;
    jit->diagnosticsLen = 0;
    jit->diagnosticsCap = 0;
}

bool TccJitLoad(TccJit* jit, const BuiltCModule* module, char** errorMessage)
{
    if (errorMessage) *errorMessage = NULL;
    if (!module || !module->source)
    {
        if (errorMessage)
        {
            *errorMessage = DupString("no C module to JIT");
        }

        return false;
    }

    jit->state = tcc_new();
    if (!jit->state)
    {
        if (errorMessage)
        {
            *errorMessage = DupString("could not create TinyCC state");
        }

        return false;
    }

    tcc_set_error_func(jit->state, jit, TccError);

    if (tcc_set_output_type(jit->state, TCC_OUTPUT_MEMORY) < 0
        || tcc_set_options(jit->state, "-nostdlib -nostdinc") < 0
        || tcc_compile_string(jit->state, module->source) < 0)
    {
        if (errorMessage)
        {
            *errorMessage = DupString(jit->diagnosticsLen ? jit->diagnostics : "TinyCC compilation failed");
        }
        return false;
    }

    tcc_add_symbol(jit->state, "memset", (const void*)(uintptr_t)&memset);
    tcc_add_symbol(jit->state, "memcpy", (const void*)(uintptr_t)&memcpy);
    tcc_add_symbol(jit->state, "memmove", (const void*)(uintptr_t)&memmove);
    tcc_add_symbol(jit->state, "fmodf", (const void*)(uintptr_t)&fmodf);
    tcc_add_symbol(jit->state, "fmod", (const void*)(uintptr_t)&fmod);
    tcc_add_symbol(jit->state, "strata_alloc", (const void*)(uintptr_t)&strata_alloc_impl);
    tcc_add_symbol(jit->state, "strata_free", (const void*)(uintptr_t)&strata_free_impl);

    if (tcc_relocate(jit->state) < 0)
    {
        if (errorMessage)
        {
            *errorMessage = DupString(jit->diagnosticsLen ? jit->diagnostics : "TinyCC relocation failed");
        }

        return false;
    }

    jit->relocated = true;

    CopySymbols(jit, &jit->exports, &module->exports);
    CopySymbols(jit, &jit->externs, &module->externs);

    /* Boxes any box globals, if the emitted module has any. */
    void (*moduleInit)(void) = (void (*)(void))tcc_get_symbol(jit->state, "__strata_module_init");

    if (moduleInit)
    {
        moduleInit();
    }

    return true;
}

bool TccJitAddSymbol(TccJit* jit, const char* name, void* address)
{
    if (!jit || !jit->state || !name || !address)
    {
        return false;
    }

    TccJitSymbol* symbol = FindSymbol(&jit->externs, name);
    if (!symbol)
    {
        return false;
    }

    void* slot = tcc_get_symbol(jit->state, symbol->cName);
    if (!slot)
    {
        return false;
    }

    memcpy(slot, &address, sizeof(address));

    return true;
}

void* TccJitGetAddress(TccJit* jit, const char* name)
{
    if (!jit || !jit->state || !name)
    {
        return NULL;
    }

    TccJitSymbol* symbol = FindSymbol(&jit->exports, name);
    if (!symbol)
    {
        return NULL;
    }

    return tcc_get_symbol(jit->state, symbol->cName);
}

bool TccJitCanInvokeIntVoid(const TccJit* jit, const char* name)
{
    if (!jit || !name)
    {
        return false;
    }

    TccJitSymbol* symbol = FindSymbol(&jit->exports, name);

    return symbol && symbol->isIntVoid;
}

size_t TccJitExternCount(const TccJit* jit)
{
    return jit ? jit->externs.count : 0;
}

const char* TccJitExternName(const TccJit* jit, size_t index)
{
    if (!jit || index >= jit->externs.count)
    {
        return NULL;
    }

    return ((const TccJitSymbol*)VecGet(&jit->externs, index))->strataName;
}

const char* TccJitDiagnostics(const TccJit* jit)
{
    return jit && jit->diagnostics ? jit->diagnostics : "";
}
