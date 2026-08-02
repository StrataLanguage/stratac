#include "Util.h"
#include "Codegen/CodegenBackend.h"
#include "Test.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static char* LoadSample(const char* name)
{
    const char* dir = STRATA_SAMPLE_DIR;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

STRATA_TEST(sample_hello_compiles_to_llvm_ir)
{
    char* src = LoadSample("hello.strata");
    STRATA_CHECK(src != NULL);
    STRATA_CHECK(src[0] != '\0');

    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count >= 3);

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @add") != NULL);
    STRATA_CHECK(strstr(res.output, "call i32") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    free(src);
}

STRATA_TEST(sample_structs_compile_with_native_backend)
{
    char* src = LoadSample("structs.strata");
    STRATA_CHECK(src != NULL);
    STRATA_CHECK(src[0] != '\0');

    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->structs.count > 0);

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "%struct.Vec3") != NULL);
    STRATA_CHECK(strstr(res.output, "%struct.Particle") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    free(src);
}

STRATA_TEST(sample_control_flow_lowers_in_llvm_backend)
{
    char* src = LoadSample("control_flow.strata");
    STRATA_CHECK(src != NULL);
    STRATA_CHECK(src[0] != '\0');

    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "br label") != NULL);
    STRATA_CHECK(strstr(res.output, "call i32 @fibonacci") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    free(src);
}

STRATA_TEST(sample_strings_compiles_to_llvm_ir)
{
    char* src = LoadSample("strings.strata");
    STRATA_CHECK(src != NULL);
    STRATA_CHECK(src[0] != '\0');

    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(src, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count >= 5);

    CodegenResult res = GenerateLlvmIr(mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(strstr(res.output, "define i32 @run") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    free(src);
}

