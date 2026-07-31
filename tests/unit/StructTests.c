#include "Util.h"
#include "strata/Test.h"
#include "strata/strata.h"

#include <stdint.h>
#include <stdio.h>

STRATA_TEST(parser_struct_declaration)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Vec3 { float x; float y; float z; };\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 1);

    StructDecl* s = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(strcmp(s->name, "Vec3") == 0);
    STRATA_CHECK_EQ((long)s->fields.count, 3);

    FieldDecl* f0 = (FieldDecl*)VecGet(&s->fields, 0);
    STRATA_CHECK(strcmp(f0->name, "x") == 0);
    STRATA_CHECK(strcmp(f0->type.name, "float") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_opaque_struct)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("handle Entity;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->handles.count, 1);

    HandleDecl* h = (HandleDecl*)VecGet(&mod->handles, 0);
    STRATA_CHECK(strcmp(h->name, "Entity") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_struct_typed_params_and_members)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(in Vec3 a, in Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->functions.count, 1);

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    ParamDecl* p0 = (ParamDecl*)VecGet(&fn->params, 0);
    STRATA_CHECK(strcmp(p0->type.name, "Vec3") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}


static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "structs", &err);
    if (err)
    {
        strataFree((char*)err);
    }
    return jit;
}

STRATA_TEST(jit_struct_member_read_write)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v;\n"
                                "  v.x = 1.0;\n"
                                "  v.y = 2.0;\n"
                                "  v.z = 3.0;\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 5.9f && r < 6.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_constructor_and_return)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "Vec3 make(float a, float b, float c) { return Vec3(a, b, c); }\n"
                                "float entry() {\n"
                                "  Vec3 v = make(10.0, 20.0, 30.0);\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_passed_between_strata_functions)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
                                "float entry() {\n"
                                "  Vec3 a; a.x = 1.0; a.y = 2.0; a.z = 3.0;\n"
                                "  Vec3 b; b.x = 4.0; b.y = 5.0; b.z = 6.0;\n"
                                "  return dot(a, b);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 31.9f && r < 32.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_nested_and_mixed_fields)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "struct Body { int id; Vec3 pos; };\n"
                                "float entry() {\n"
                                "  Body b;\n"
                                "  b.id = 7;\n"
                                "  b.pos = Vec3(1.0, 2.0, 3.0);\n"
                                "  return b.pos.x + b.pos.y + b.pos.z + b.id;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 12.9f && r < 13.1f);
        }
        strataJitDestroy(jit);
    }
}

static void* test_spawn(void)
{
    return (void*)(intptr_t)0xC0FFEE;
}

static void test_despawn(void* e)
{
    (void)e;
}

static int test_id_of(void* e)
{
    return (int)(intptr_t)e;
}

STRATA_TEST(jit_opaque_engine_handle)
{
    StrataJit* jit = CompileJit("handle Entity;\n"
                                "extern Entity spawn();\n"
                                "extern void despawn(Entity e);\n"
                                "extern int id_of(Entity e);\n"
                                "int run() {\n"
                                "  Entity e = spawn();\n"
                                "  int i = id_of(e);\n"
                                "  despawn(e);\n"
                                "  return i;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return;
    }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "spawn", (void*)&test_spawn), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "despawn", (void*)&test_despawn), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "id_of", (void*)&test_id_of), 1);

    int (*run)(void) = (int (*)(void))strataJitGetFunction(jit, "run");
    STRATA_CHECK(run != NULL);
    if (run)
    {
        STRATA_CHECK_EQ(run(), (long)0xC0FFEE);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(jit_struct_zero_initialized_by_default)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v;\n"
                                "  v.x = 7.0;\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 6.9f && r < 7.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_inout_param_is_by_reference)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "void bump(inout Vec3 v) { v.x = v.x + 10.0; v.y = v.y + 20.0; }\n"
                                "float entry() {\n"
                                "  Vec3 a; a.x = 1.0; a.y = 2.0; a.z = 3.0;\n"
                                "  bump(a);\n"
                                "  return a.x + a.y + a.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 35.9f && r < 36.1f);
        }
        strataJitDestroy(jit);
    }
}

typedef struct { float x, y, z; } HostVec3;

static float test_length_sq(const HostVec3* v)
{
    return (v->x * v->x) + (v->y * v->y) + (v->z * v->z);
}

static void test_scale_into(const HostVec3* src, float s, HostVec3* dst)
{
    dst->x = src->x * s;
    dst->y = src->y * s;
    dst->z = src->z * s;
}

STRATA_TEST(jit_extern_struct_crosses_boundary_by_pointer)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "extern float length_sq(in Vec3 v);\n"
                                "extern void scale_into(in Vec3 src, float s, out Vec3 dst);\n"
                                "float entry() {\n"
                                "  Vec3 v = Vec3(3.0, 4.0, 0.0);\n"
                                "  Vec3 r;\n"
                                "  scale_into(v, 2.0, r);\n"
                                "  return length_sq(v) + length_sq(r);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        return;
    }

    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "length_sq", (void*)&test_length_sq), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "scale_into", (void*)&test_scale_into), 1);

    float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(f != NULL);
    if (f)
    {
        float r = f();
        STRATA_CHECK(r > 124.9f && r < 125.1f);
    }

    strataJitDestroy(jit);
}

#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"

STRATA_TEST(aot_emits_struct_object)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false);
    const char* path = "strata_struct_test.o";
    char* err = NULL;
    bool ok = EmitNativeFile(&bm, path, false, &err, NULL);
    STRATA_CHECK(ok);

    FILE* in = fopen(path, "rb");
    STRATA_CHECK(in != NULL);
    if (in)
    {
        fseek(in, 0, SEEK_END);
        STRATA_CHECK(ftell(in) > 0);
        fclose(in);
    }

    BuiltModuleDispose(&bm);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(jit_braced_init_positional)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = Vec3{10.0, 20.0, 30.0};\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_braced_init_designated)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = Vec3{.x = 10.0, .y = 20.0, .z = 30.0};\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_braced_init_partial_designated)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = Vec3{.z = 42.0};\n"
                                "  return v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 41.9f && r < 42.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_braced_init_out_of_order)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = Vec3{.z = 3.0, .x = 1.0, .y = 2.0};\n"
                                "  return v.x * 100.0 + v.y * 10.0 + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 122.9f && r < 123.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_braced_init_inferred_type)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = {.x = 10.0, .y = 20.0, .z = 30.0};\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_braced_init_inferred_positional)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float entry() {\n"
                                "  Vec3 v = {10.0, 20.0, 30.0};\n"
                                "  return v.x + v.y + v.z;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_in_scalar_param_reads_correctly)
{
    StrataJit* jit = CompileJit("int identity(in int x) { return x; }\n"
                                "int entry() { return identity(42); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 42);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_in_float_param_reads_correctly)
{
    StrataJit* jit = CompileJit("float half_val(in float x) { return x * 0.5; }\n"
                                "float entry() { return half_val(10.0); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 4.9f && r < 5.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_in_param_does_not_corrupt_caller)
{
    StrataJit* jit = CompileJit("void consume(in int x) { int unused = x + 1; }\n"
                                "int entry() {\n"
                                "  int v = 99;\n"
                                "  consume(v);\n"
                                "  return v;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 99);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_multiple_in_params)
{
    StrataJit* jit = CompileJit("int sum(in int a, in int b, in int c) { return a + b + c; }\n"
                                "int entry() { return sum(10, 20, 30); }\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 60);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_in_struct_param_passed_by_ref)
{
    StrataJit* jit = CompileJit("struct Vec3 { float x; float y; float z; };\n"
                                "float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
                                "float entry() {\n"
                                "  Vec3 a = {1.0, 2.0, 3.0};\n"
                                "  Vec3 b = {4.0, 5.0, 6.0};\n"
                                "  return dot(a, b);\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        float (*f)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            float r = f();
            STRATA_CHECK(r > 31.9f && r < 32.1f);
        }
        strataJitDestroy(jit);
    }
}

