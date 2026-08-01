#include "Codegen/CBackend.h"
#include "Codegen/LLVMJit.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/TccJit.h"
#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Lex/Lexer.h"
#include "Parse/Parser.h"
#include "Sema/ResolveOverloads.h"
#include "strata/strata.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef STRATA_BUILD_TYPE
#define STRATA_BUILD_TYPE "unknown"
#endif
#ifndef STRATA_SYSTEM_NAME
#define STRATA_SYSTEM_NAME "unknown"
#endif
#ifndef STRATA_SYSTEM_PROCESSOR
#define STRATA_SYSTEM_PROCESSOR "unknown"
#endif
#ifndef STRATA_C_COMPILER
#define STRATA_C_COMPILER "unknown"
#endif

typedef enum {
    BackendLlvm,
    BackendTcc,
} Backend;

typedef enum {
    ShapeArithmetic,
    ShapeControlFlow,
    ShapeStructs,
    ShapeOverloads,
} FixtureShape;

typedef struct {
    char path[1024];
    char name[128];
    FixtureShape shape;
    size_t bytes;
    size_t lines;
    size_t declarations;
    size_t functions;
    int expected;
} Fixture;

typedef struct {
    double total;
    double build;
    double relocate;
    bool ok;
} Timing;

typedef struct {
    double minimum;
    double median;
    double p95;
} Summary;

typedef struct {
    char fixtureName[128];
    Summary llvm;
    Summary tcc;
    size_t llvmCalls;
    size_t tccCalls;
    size_t samples;
} RuntimeReport;

typedef struct {
    Backend backend;
    LLVMJit llvm;
    TccJit tcc;
    uint32_t (*hot)(uint32_t, int);
} LoadedScript;

typedef struct {
    FILE* file;
    size_t bytes;
    size_t lines;
} FixtureWriter;

static double NowSeconds(void)
{
    struct timespec value;
    timespec_get(&value, TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static volatile uint32_t runtimeSink;

static bool WriteText(FixtureWriter* writer, const char* text)
{
    size_t length = strlen(text);
    if (fwrite(text, 1, length, writer->file) != length) return false;
    writer->bytes += length;
    for (size_t i = 0; i < length; ++i)
    {
        if (text[i] == '\n') writer->lines++;
    }
    return true;
}

static bool PadFixture(FixtureWriter* writer, size_t targetBytes)
{
    if (writer->bytes > targetBytes) return false;
    size_t remaining = targetBytes - writer->bytes;
    if (remaining >= 4)
    {
        if (!WriteText(writer, "/*")) return false;
        remaining -= 4;
        char block[4096];
        memset(block, 'x', sizeof(block));
        while (remaining)
        {
            size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
            if (fwrite(block, 1, count, writer->file) != count) return false;
            writer->bytes += count;
            remaining -= count;
        }
        return WriteText(writer, "*/");
    }
    while (remaining--)
    {
        if (fputc(' ', writer->file) == EOF) return false;
        writer->bytes++;
    }
    return true;
}

static const char* ShapeName(FixtureShape shape)
{
    switch (shape)
    {
    case ShapeArithmetic: return "arithmetic";
    case ShapeControlFlow: return "control";
    case ShapeStructs: return "structs";
    case ShapeOverloads: return "overloads";
    }
    return "unknown";
}

static bool GenerateFixture(const char* directory, size_t mib, FixtureShape shape,
                            Fixture* fixture)
{
    size_t targetBytes = mib * 1024 * 1024;
    size_t operations = targetBytes / 4096;
    if (operations < 1) operations = 1;

    snprintf(fixture->name, sizeof(fixture->name), "%s-%zuMiB", ShapeName(shape), mib);
    snprintf(fixture->path, sizeof(fixture->path), "%s/%s.strata", directory, fixture->name);
    fixture->shape = shape;
    fixture->declarations = 1;
    fixture->functions = 1;

    FixtureWriter writer = {0};
    writer.file = fopen(fixture->path, "wb");
    if (!writer.file)
    {
        fprintf(stderr, "cannot create fixture %s\n", fixture->path);
        return false;
    }

    bool ok = true;
    if (shape == ShapeArithmetic)
    {
        ok = WriteText(&writer, "int entry() {\n  int total = 0;\n");
        for (size_t i = 0; ok && i < operations; ++i)
            ok = WriteText(&writer, "  total = total + 1;\n");
        ok = ok && WriteText(&writer, "  return total;\n}\n");
        fixture->expected = (int)operations;
    }
    else if (shape == ShapeControlFlow)
    {
        ok = WriteText(&writer, "int entry() {\n  int total = 0;\n");
        for (size_t i = 0; ok && i < operations; ++i)
        {
            ok = WriteText(&writer,
                "  if ((total & 1) == 0) { total = total + 3; } "
                "else { total = total + 1; }\n");
        }
        ok = ok && WriteText(&writer, "  return total;\n}\n");
        fixture->expected = (int)(operations * 2 + (operations & 1));
    }
    else if (shape == ShapeStructs)
    {
        ok = WriteText(&writer,
            "struct Pair { int a; int b; };\n"
            "int entry() {\n  Pair p = {.a = 0, .b = 0};\n");
        for (size_t i = 0; ok && i < operations; ++i)
            ok = WriteText(&writer, "  p.a += 1; p.b += 2;\n");
        ok = ok && WriteText(&writer, "  return p.a + p.b;\n}\n");
        fixture->declarations = 2;
        fixture->expected = (int)(operations * 3);
    }
    else
    {
        ok = WriteText(&writer,
            "int mix(int a, int b) { return a + b; }\n"
            "float mix(float a, float b) { return a * b; }\n"
            "int entry() {\n  int total = 0;\n");
        for (size_t i = 0; ok && i < operations; ++i)
            ok = WriteText(&writer, "  total += mix(1, 2);\n");
        ok = ok && WriteText(&writer, "  return total;\n}\n");
        fixture->declarations = 3;
        fixture->functions = 3;
        fixture->expected = (int)(operations * 3);
    }

    if (ok && shape == ShapeArithmetic)
    {
        ok = WriteText(&writer,
            "uint hot(uint state, int rounds) {\n"
            "  for (int i = 0; i < rounds; i++) {\n"
            "    state = state * 1664525u + 1013904223u;\n"
            "    state = state ^ (state >> 16);\n"
            "  }\n"
            "  return state;\n"
            "}\n");
        fixture->declarations++;
        fixture->functions++;
    }
    else if (ok && shape == ShapeControlFlow)
    {
        ok = WriteText(&writer,
            "uint hot(uint state, int rounds) {\n"
            "  for (int i = 0; i < rounds; i++) {\n"
            "    if ((state & 1u) == 0u) { state = (state >> 1) ^ 277803737u; }\n"
            "    else { state = state * 3u + 1u; }\n"
            "    state = state ^ (state << 7);\n"
            "  }\n"
            "  return state;\n"
            "}\n");
        fixture->declarations++;
        fixture->functions++;
    }
    else if (ok && shape == ShapeStructs)
    {
        ok = WriteText(&writer,
            "struct HotPair { uint a; uint b; };\n"
            "uint hot(uint state, int rounds) {\n"
            "  HotPair p = {.a = state, .b = state ^ 277803737u};\n"
            "  for (int i = 0; i < rounds; i++) {\n"
            "    p.a = p.a * 1664525u + p.b;\n"
            "    p.b = (p.b << 5) ^ (p.a >> 3) ^ 1013904223u;\n"
            "  }\n"
            "  return p.a ^ p.b;\n"
            "}\n");
        fixture->declarations += 2;
        fixture->functions++;
    }
    else if (ok)
    {
        ok = WriteText(&writer,
            "uint mix(uint a, uint b) { return a * 1664525u + b; }\n"
            "uint hot(uint state, int rounds) {\n"
            "  for (int i = 0; i < rounds; i++) {\n"
            "    state = mix(state, (uint)i + 1013904223u);\n"
            "    state = state ^ (state >> 15);\n"
            "  }\n"
            "  return state;\n"
            "}\n");
        fixture->declarations += 2;
        fixture->functions += 2;
    }

    ok = ok && PadFixture(&writer, targetBytes);
    ok = fclose(writer.file) == 0 && ok;
    fixture->bytes = writer.bytes;
    fixture->lines = writer.lines;
    if (!ok || fixture->bytes != targetBytes)
    {
        fprintf(stderr, "failed to generate exact-size fixture %s (%zu/%zu bytes)\n",
                fixture->path, fixture->bytes, targetBytes);
        return false;
    }
    return true;
}

static char* ReadFile(const char* path, size_t* lengthOut)
{
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char* source = (char*)malloc((size_t)size + 1);
    if (!source)
    {
        fclose(file);
        return NULL;
    }
    size_t length = fread(source, 1, (size_t)size, file);
    fclose(file);
    source[length] = '\0';
    if (lengthOut) *lengthOut = length;
    return source;
}

static Module* ParseSource(const char* source, size_t length, const char* name,
                           Arena* arena, DiagnosticEngine* diag, SourceManager* manager)
{
    SourceManagerInit(manager);
    SourceManagerSetSource(manager, source, length, name);
    Lexer lexer;
    LexerInit(&lexer, manager->m_text, manager->m_textLen, diag, 0);
    Parser parser;
    ParserInit(&parser, &lexer, diag, arena, name);
    Module* module = ParserParseModule(&parser);
    ResolveOverloads(module, diag, arena);
    return module;
}

static Timing CompileAst(Module* module, Backend backend, int expected)
{
    Timing timing = {0};
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    double totalStart = NowSeconds();
    double buildStart = totalStart;
    if (backend == BackendLlvm)
    {
        BuiltModule built = BuildLlvmModule(module, &diag, &arena, true);
        double buildEnd = NowSeconds();
        LLVMJit jit;
        LLVMJitInit(&jit);
        char* error = NULL;
        bool loaded = !DiagHasErrors(&diag) && LLVMJitLoad(&jit, &built, &error);
        double relocateEnd = NowSeconds();
        timing.total = relocateEnd - totalStart;
        timing.build = buildEnd - buildStart;
        timing.relocate = relocateEnd - buildEnd;
        if (loaded)
        {
            int (*entry)(void) = (int (*)(void))(uintptr_t)LLVMJitGetAddress(&jit, "entry");
            int actual = entry ? entry() : 0;
            timing.ok = entry && actual == expected;
            if (!timing.ok)
                fprintf(stderr, "LLVM entry returned %d, expected %d\n", actual, expected);
        }
        if (!timing.ok && error) fprintf(stderr, "LLVM JIT: %s\n", error);
        free(error);
        LLVMJitDestroy(&jit);
        BuiltModuleDispose(&built);
    }
    else
    {
        BuiltCModule built = BuildCModule(module, &diag, &arena, true);
        double buildEnd = NowSeconds();
        TccJit jit;
        TccJitInit(&jit);
        char* error = NULL;
        bool loaded = !DiagHasErrors(&diag) && TccJitLoad(&jit, &built, &error);
        double relocateEnd = NowSeconds();
        timing.total = relocateEnd - totalStart;
        timing.build = buildEnd - buildStart;
        timing.relocate = relocateEnd - buildEnd;
        if (loaded)
        {
            int (*entry)(void) = (int (*)(void))TccJitGetAddress(&jit, "entry");
            int actual = entry ? entry() : 0;
            timing.ok = entry && actual == expected;
            if (!timing.ok)
                fprintf(stderr, "TinyCC entry returned %d, expected %d\n", actual, expected);
        }
        if (!timing.ok && error) fprintf(stderr, "TinyCC JIT: %s\n", error);
        free(error);
        TccJitDestroy(&jit);
        BuiltCModuleDispose(&built);
    }

    if (DiagHasErrors(&diag))
    {
        fprintf(stderr, "%s backend diagnostics: %u error(s)\n",
                backend == BackendLlvm ? "LLVM" : "TinyCC", DiagErrorCount(&diag));
    }
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    return timing;
}

static Timing CompileSource(const char* source, size_t length, const char* name,
                            Backend backend, int expected)
{
    double start = NowSeconds();
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    SourceManager manager;
    Module* module = ParseSource(source, length, name, &arena, &diag, &manager);
    Timing backendTiming = {0};
    if (!DiagHasErrors(&diag)) backendTiming = CompileAst(module, backend, expected);
    double end = NowSeconds();
    AstDispose((Node*)module);
    SourceManagerFree(&manager);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    backendTiming.total = end - start;
    return backendTiming;
}

static Timing CompileFile(const Fixture* fixture, Backend backend)
{
    double start = NowSeconds();
    size_t length = 0;
    char* source = ReadFile(fixture->path, &length);
    Timing timing = {0};
    if (source)
    {
        timing = CompileSource(source, length, fixture->name, backend, fixture->expected);
        free(source);
    }
    double end = NowSeconds();
    timing.total = end - start;
    return timing;
}

static int CompareDouble(const void* lhs, const void* rhs)
{
    double a = *(const double*)lhs;
    double b = *(const double*)rhs;
    return a < b ? -1 : a > b ? 1 : 0;
}

static Summary Summarize(const double* input, size_t count)
{
    double* values = (double*)malloc(count * sizeof(double));
    memcpy(values, input, count * sizeof(double));
    qsort(values, count, sizeof(double), CompareDouble);
    Summary result;
    result.minimum = values[0];
    result.median = count & 1
        ? values[count / 2]
        : (values[count / 2 - 1] + values[count / 2]) * 0.5;
    size_t p95Index = (size_t)ceil((double)count * 0.95) - 1;
    if (p95Index >= count) p95Index = count - 1;
    result.p95 = values[p95Index];
    free(values);
    return result;
}

static void LoadedScriptInit(LoadedScript* script, Backend backend)
{
    memset(script, 0, sizeof(*script));
    script->backend = backend;
    LLVMJitInit(&script->llvm);
    TccJitInit(&script->tcc);
}

static void LoadedScriptDestroy(LoadedScript* script)
{
    LLVMJitDestroy(&script->llvm);
    TccJitDestroy(&script->tcc);
    script->hot = NULL;
}

static bool LoadScript(Module* module, Backend backend, LoadedScript* script)
{
    LoadedScriptInit(script, backend);
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    char* error = NULL;
    bool loaded = false;

    if (backend == BackendLlvm)
    {
        BuiltModule built = BuildLlvmModule(module, &diag, &arena, true);
        loaded = !DiagHasErrors(&diag) && LLVMJitLoad(&script->llvm, &built, &error);
        if (loaded)
            script->hot = (uint32_t (*)(uint32_t, int))(uintptr_t)
                LLVMJitGetAddress(&script->llvm, "hot");
        BuiltModuleDispose(&built);
    }
    else
    {
        BuiltCModule built = BuildCModule(module, &diag, &arena, true);
        loaded = !DiagHasErrors(&diag) && TccJitLoad(&script->tcc, &built, &error);
        if (loaded)
            script->hot = (uint32_t (*)(uint32_t, int))TccJitGetAddress(&script->tcc, "hot");
        BuiltCModuleDispose(&built);
    }

    if (!loaded || !script->hot)
    {
        fprintf(stderr, "%s runtime load failed: %s\n",
                backend == BackendLlvm ? "LLVM" : "TinyCC",
                error ? error : "hot symbol not found");
    }
    free(error);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    return loaded && script->hot;
}

static uint32_t ReferenceHot(FixtureShape shape, uint32_t state, int rounds)
{
    uint32_t pairA = state;
    uint32_t pairB = state ^ UINT32_C(277803737);
    for (int i = 0; i < rounds; ++i)
    {
        switch (shape)
        {
        case ShapeArithmetic:
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            state ^= state >> 16;
            break;
        case ShapeControlFlow:
            state = (state & 1u) == 0u
                ? (state >> 1) ^ UINT32_C(277803737)
                : state * 3u + 1u;
            state ^= state << 7;
            break;
        case ShapeStructs:
            pairA = pairA * UINT32_C(1664525) + pairB;
            pairB = (pairB << 5) ^ (pairA >> 3) ^ UINT32_C(1013904223);
            break;
        case ShapeOverloads:
            state = state * UINT32_C(1664525) + (uint32_t)i + UINT32_C(1013904223);
            state ^= state >> 15;
            break;
        }
    }
    return shape == ShapeStructs ? pairA ^ pairB : state;
}

static double RunHotBatch(uint32_t (*hot)(uint32_t, int), size_t calls, int rounds,
                          uint32_t seed, uint32_t* resultOut)
{
    uint32_t result = seed;
    double start = NowSeconds();
    for (size_t i = 0; i < calls; ++i) result = hot(result, rounds);
    double elapsed = NowSeconds() - start;
    runtimeSink = result;
    if (resultOut) *resultOut = result;
    return elapsed;
}

static size_t CalibrateRuntime(uint32_t (*hot)(uint32_t, int), int rounds)
{
    const double targetSeconds = 0.050;
    const size_t maximumCalls = 100000000;
    size_t calls = 16;
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        uint32_t result = 0;
        double elapsed = RunHotBatch(hot, calls, rounds, UINT32_C(0x12345678), &result);
        if (elapsed >= targetSeconds || calls == maximumCalls) return calls;

        double scale = elapsed > 0.0 ? targetSeconds / elapsed : 10.0;
        if (scale < 2.0) scale = 2.0;
        if (scale > 10.0) scale = 10.0;
        size_t next = (size_t)((double)calls * scale);
        if (next <= calls) next = calls + 1;
        calls = next > maximumCalls ? maximumCalls : next;
    }
    return calls;
}

static bool MeasureRuntime(FILE* csv, const Fixture* fixture, Module* ast,
                           size_t samples, RuntimeReport* report)
{
    const int rounds = 4096;
    const uint32_t validationSeed = UINT32_C(0x12345678);
    LoadedScript scripts[2];
    bool llvmLoaded = LoadScript(ast, BackendLlvm, &scripts[BackendLlvm]);
    bool tccLoaded = LoadScript(ast, BackendTcc, &scripts[BackendTcc]);
    if (!llvmLoaded || !tccLoaded)
    {
        LoadedScriptDestroy(&scripts[BackendLlvm]);
        LoadedScriptDestroy(&scripts[BackendTcc]);
        return false;
    }

    uint32_t expected = ReferenceHot(fixture->shape, validationSeed, rounds);
    for (int i = 0; i < 8; ++i)
    {
        if (scripts[BackendLlvm].hot(validationSeed, rounds) != expected
            || scripts[BackendTcc].hot(validationSeed, rounds) != expected)
        {
            LoadedScriptDestroy(&scripts[BackendLlvm]);
            LoadedScriptDestroy(&scripts[BackendTcc]);
            return false;
        }
    }

    size_t calls[2];
    calls[BackendLlvm] = CalibrateRuntime(scripts[BackendLlvm].hot, rounds);
    calls[BackendTcc] = CalibrateRuntime(scripts[BackendTcc].hot, rounds);
    size_t commonCalls = calls[BackendLlvm] > calls[BackendTcc]
        ? calls[BackendLlvm] : calls[BackendTcc];
    calls[BackendLlvm] = commonCalls;
    calls[BackendTcc] = commonCalls;
    double* elapsed[2];
    elapsed[BackendLlvm] = (double*)calloc(samples, sizeof(double));
    elapsed[BackendTcc] = (double*)calloc(samples, sizeof(double));
    bool ok = calls[BackendLlvm] > 0 && calls[BackendTcc] > 0
        && elapsed[BackendLlvm] && elapsed[BackendTcc];

    for (size_t sample = 0; ok && sample < samples; ++sample)
    {
        Backend first = sample & 1 ? BackendTcc : BackendLlvm;
        uint32_t results[2] = {0, 0};
        for (int order = 0; order < 2; ++order)
        {
            Backend backend = order == 0 ? first
                : (first == BackendLlvm ? BackendTcc : BackendLlvm);
            uint32_t seed = validationSeed + (uint32_t)sample;
            double batch = RunHotBatch(scripts[backend].hot, calls[backend], rounds,
                                       seed, &results[backend]);
            elapsed[backend][sample] = batch / (double)calls[backend];
        }
        ok = results[BackendLlvm] == results[BackendTcc];
    }

    if (ok)
    {
        snprintf(report->fixtureName, sizeof(report->fixtureName), "%s", fixture->name);
        report->llvm = Summarize(elapsed[BackendLlvm], samples);
        report->tcc = Summarize(elapsed[BackendTcc], samples);
        report->llvmCalls = calls[BackendLlvm];
        report->tccCalls = calls[BackendTcc];
        report->samples = samples;
        double ratio = report->llvm.median > 0.0
            ? report->tcc.median / report->llvm.median : 0.0;
        if (csv)
        {
            fprintf(csv, "%s,%zu,%zu,%zu,%zu,hot-runtime,LLVM,%.9f,%.9f,%.9f,,%zu,%.6f,%.3f,%zu\n",
                    fixture->name, fixture->bytes, fixture->lines, fixture->declarations,
                    fixture->functions, report->llvm.minimum * 1000.0,
                    report->llvm.median * 1000.0, report->llvm.p95 * 1000.0,
                    samples, ratio, 1.0 / report->llvm.median, report->llvmCalls);
            fprintf(csv, "%s,%zu,%zu,%zu,%zu,hot-runtime,TinyCC,%.9f,%.9f,%.9f,,%zu,%.6f,%.3f,%zu\n",
                    fixture->name, fixture->bytes, fixture->lines, fixture->declarations,
                    fixture->functions, report->tcc.minimum * 1000.0,
                    report->tcc.median * 1000.0, report->tcc.p95 * 1000.0,
                    samples, ratio, 1.0 / report->tcc.median, report->tccCalls);
        }
    }

    free(elapsed[BackendLlvm]);
    free(elapsed[BackendTcc]);
    LoadedScriptDestroy(&scripts[BackendLlvm]);
    LoadedScriptDestroy(&scripts[BackendTcc]);
    return ok;
}

static void PrintRuntimeReports(const RuntimeReport* reports, size_t count)
{
    printf("\nHot execution after JIT load (compilation excluded, 4096 rounds/call)\n");
    printf("%-22s %12s %12s %12s %12s %9s\n",
           "fixture", "LLVM ns/call", "TCC ns/call", "LLVM calls/s", "TCC calls/s", "ratio");
    for (size_t i = 0; i < count; ++i)
    {
        const RuntimeReport* report = &reports[i];
        double ratio = report->llvm.median > 0.0
            ? report->tcc.median / report->llvm.median : 0.0;
        printf("%-22s %12.1f %12.1f %12.0f %12.0f %8.2fx\n",
               report->fixtureName, report->llvm.median * 1000000000.0,
               report->tcc.median * 1000000000.0,
               1.0 / report->llvm.median, 1.0 / report->tcc.median, ratio);
    }
}

static void PrintPair(FILE* csv, const Fixture* fixture, const char* view,
                      const double* llvm, const double* tcc, size_t iterations)
{
    Summary llvmSummary = Summarize(llvm, iterations);
    Summary tccSummary = Summarize(tcc, iterations);
    double mib = (double)fixture->bytes / (1024.0 * 1024.0);
    double ratio = llvmSummary.median > 0 ? tccSummary.median / llvmSummary.median : 0;
    printf("%-22s %-18s %9.3f %9.3f %9.3f %9.3f %8.2fx\n",
           fixture->name, view,
           llvmSummary.median * 1000.0, tccSummary.median * 1000.0,
           mib / llvmSummary.median, mib / tccSummary.median, ratio);

    if (csv)
    {
        fprintf(csv, "%s,%zu,%zu,%zu,%zu,%s,LLVM,%.6f,%.6f,%.6f,%.3f,%zu,%.6f,,\n",
                fixture->name, fixture->bytes, fixture->lines, fixture->declarations,
                fixture->functions, view, llvmSummary.minimum * 1000.0,
                llvmSummary.median * 1000.0, llvmSummary.p95 * 1000.0,
                mib / llvmSummary.median, iterations, ratio);
        fprintf(csv, "%s,%zu,%zu,%zu,%zu,%s,TinyCC,%.6f,%.6f,%.6f,%.3f,%zu,%.6f,,\n",
                fixture->name, fixture->bytes, fixture->lines, fixture->declarations,
                fixture->functions, view, tccSummary.minimum * 1000.0,
                tccSummary.median * 1000.0, tccSummary.p95 * 1000.0,
                mib / tccSummary.median, iterations, ratio);
    }
}

static bool MeasureView(FILE* csv, const Fixture* fixture, const char* source,
                        size_t length, Module* ast, const char* view, size_t iterations)
{
    double* llvm = (double*)calloc(iterations, sizeof(double));
    double* tcc = (double*)calloc(iterations, sizeof(double));
    double* llvmBuild = (double*)calloc(iterations, sizeof(double));
    double* tccBuild = (double*)calloc(iterations, sizeof(double));
    double* llvmRelocate = (double*)calloc(iterations, sizeof(double));
    double* tccRelocate = (double*)calloc(iterations, sizeof(double));

    Timing warmLlvm;
    Timing warmTcc;
    if (strcmp(view, "file-to-callable") == 0)
    {
        warmLlvm = CompileFile(fixture, BackendLlvm);
        warmTcc = CompileFile(fixture, BackendTcc);
    }
    else if (strcmp(view, "source-to-callable") == 0)
    {
        warmLlvm = CompileSource(source, length, fixture->name, BackendLlvm, fixture->expected);
        warmTcc = CompileSource(source, length, fixture->name, BackendTcc, fixture->expected);
    }
    else
    {
        warmLlvm = CompileAst(ast, BackendLlvm, fixture->expected);
        warmTcc = CompileAst(ast, BackendTcc, fixture->expected);
    }
    bool ok = warmLlvm.ok && warmTcc.ok;

    for (size_t i = 0; ok && i < iterations; ++i)
    {
        Timing results[2];
        Backend first = i & 1 ? BackendTcc : BackendLlvm;
        for (int order = 0; order < 2; ++order)
        {
            Backend backend = order == 0 ? first : (first == BackendLlvm ? BackendTcc : BackendLlvm);
            if (strcmp(view, "file-to-callable") == 0)
                results[backend] = CompileFile(fixture, backend);
            else if (strcmp(view, "source-to-callable") == 0)
                results[backend] = CompileSource(source, length, fixture->name, backend, fixture->expected);
            else
                results[backend] = CompileAst(ast, backend, fixture->expected);
        }
        ok = results[BackendLlvm].ok && results[BackendTcc].ok;
        llvm[i] = results[BackendLlvm].total;
        tcc[i] = results[BackendTcc].total;
        llvmBuild[i] = results[BackendLlvm].build;
        tccBuild[i] = results[BackendTcc].build;
        llvmRelocate[i] = results[BackendLlvm].relocate;
        tccRelocate[i] = results[BackendTcc].relocate;
    }

    if (ok)
    {
        PrintPair(csv, fixture, view, llvm, tcc, iterations);
        if (strcmp(view, "ast-to-callable") == 0)
        {
            PrintPair(csv, fixture, "backend-build", llvmBuild, tccBuild, iterations);
            PrintPair(csv, fixture, "engine-relocate", llvmRelocate, tccRelocate, iterations);
        }
    }

    free(llvm);
    free(tcc);
    free(llvmBuild);
    free(tccBuild);
    free(llvmRelocate);
    free(tccRelocate);
    return ok;
}

static size_t ParseSizes(const char* text, size_t* sizes, size_t capacity)
{
    size_t count = 0;
    const char* cursor = text;
    while (*cursor && count < capacity)
    {
        char* end = NULL;
        unsigned long value = strtoul(cursor, &end, 10);
        if (end == cursor || value == 0) return 0;
        sizes[count++] = (size_t)value;
        cursor = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') return 0;
    }
    return count;
}

int main(int argc, char** argv)
{
    const char* fixtureDirectory = STRATA_BENCH_FIXTURE_DIR;
    const char* csvPath = NULL;
    size_t sizes[16] = {1, 5, 20};
    size_t sizeCount = 3;
    size_t iterations = 3;
    bool runtimeOnly = false;
    RuntimeReport runtimeReports[64];
    size_t runtimeReportCount = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--quick") == 0)
        {
            sizes[0] = 1;
            sizeCount = 1;
            iterations = 1;
        }
        else if (strcmp(argv[i], "--runtime-only") == 0)
        {
            runtimeOnly = true;
        }
        else if (strcmp(argv[i], "--sizes") == 0 && i + 1 < argc)
        {
            sizeCount = ParseSizes(argv[++i], sizes, 16);
            if (!sizeCount)
            {
                fprintf(stderr, "invalid --sizes list\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
        {
            iterations = (size_t)strtoul(argv[++i], NULL, 10);
            if (!iterations) return 2;
        }
        else if (strcmp(argv[i], "--fixture-dir") == 0 && i + 1 < argc)
        {
            fixtureDirectory = argv[++i];
        }
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
        {
            csvPath = argv[++i];
        }
        else
        {
            fprintf(stderr,
                "usage: strata_jit_bench [--quick] [--runtime-only] [--sizes 1,5,20] "
                "[--iterations N] [--fixture-dir DIR] [--csv FILE]\n");
            return 2;
        }
    }

    FILE* csv = csvPath ? fopen(csvPath, "wb") : NULL;
    if (csvPath && !csv)
    {
        fprintf(stderr, "cannot open CSV output %s\n", csvPath);
        return 1;
    }
    if (csv)
    {
        fprintf(csv,
            "fixture,bytes,lines,declarations,functions,view,backend,min_ms,median_ms,"
            "p95_ms,mib_per_second,iterations,tcc_over_llvm,calls_per_second,batch_calls\n");
    }

    printf("Strata JIT benchmark: build=%s system=%s arch=%s compiler=%s LLVM=%s TinyCC=0.9.28-strata seed=20260731\n",
           STRATA_BUILD_TYPE, STRATA_SYSTEM_NAME, STRATA_SYSTEM_PROCESSOR,
           STRATA_C_COMPILER, strataLLVMVersion());
    if (!runtimeOnly)
    {
        printf("%-22s %-18s %9s %9s %9s %9s %9s\n",
               "fixture", "view", "LLVM ms", "TCC ms", "LLVM MiB/s", "TCC MiB/s", "ratio");
    }

    bool ok = true;
    for (size_t s = 0; ok && s < sizeCount; ++s)
    {
        for (FixtureShape shape = ShapeArithmetic; ok && shape <= ShapeOverloads; ++shape)
        {
            Fixture fixture = {0};
            ok = GenerateFixture(fixtureDirectory, sizes[s], shape, &fixture);
            size_t length = 0;
            char* source = ok ? ReadFile(fixture.path, &length) : NULL;
            ok = ok && source && length == fixture.bytes;
            if (!ok)
            {
                free(source);
                break;
            }

            Arena astArena;
            arena_init(&astArena, 0);
            DiagnosticEngine astDiag;
            DiagnosticEngineInit(&astDiag);
            SourceManager astManager;
            Module* ast = ParseSource(source, length, fixture.name,
                                      &astArena, &astDiag, &astManager);
            ok = !DiagHasErrors(&astDiag);
            if (ok && !runtimeOnly) ok = MeasureView(csv, &fixture, source, length, ast,
                                                     "file-to-callable", iterations);
            if (ok && !runtimeOnly) ok = MeasureView(csv, &fixture, source, length, ast,
                                                     "source-to-callable", iterations);
            if (ok && !runtimeOnly) ok = MeasureView(csv, &fixture, source, length, ast,
                                                     "ast-to-callable", iterations);
            if (ok) ok = MeasureRuntime(csv, &fixture, ast, iterations,
                                        &runtimeReports[runtimeReportCount]);
            if (ok) runtimeReportCount++;
            AstDispose((Node*)ast);
            SourceManagerFree(&astManager);
            DiagnosticEngineFree(&astDiag);
            arena_free(&astArena);
            free(source);
        }
    }

    if (csv) fclose(csv);
    if (!ok)
    {
        fprintf(stderr, "benchmark aborted because a backend failed correctness validation\n");
        return 1;
    }
    PrintRuntimeReports(runtimeReports, runtimeReportCount);
    return 0;
}
