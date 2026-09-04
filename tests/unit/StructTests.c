#include "Util.h"
#include "Codegen/TypeRegistry.h"
#include "Test.h"
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

STRATA_TEST(parser_forward_struct_declaration)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Foo;\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 1);

    StructDecl* s = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(strcmp(s->name, "Foo") == 0);
    STRATA_CHECK(s->incomplete);
    STRATA_CHECK_EQ((long)s->fields.count, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(forward_struct_is_completed_by_later_body)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct Foo;\n"
        "int get_x(const Foo f) { return f.x; }\n"
        "struct Foo { int x; };\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    /* The forward decl and the body collapse to a single complete type, so a
       function declared between them may use the field. */
    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);
    const StructType* t = TypeRegistryFind(&reg, "Foo");
    STRATA_CHECK(t != NULL);
    STRATA_CHECK(!t->opaque);
    STRATA_CHECK(!t->incomplete);
    STRATA_CHECK_EQ((long)t->fields.count, 1);
    TypeRegistryFree(&reg);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(jit_forward_decl_completed_and_used)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct Foo;\n"
        "int get_x(const Foo f) { return f.x; }\n"
        "struct Foo { int x; };\n"
        "int entry() { Foo f; f.x = 42; return get_x(f); }\n",
        "fwd", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 42);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(parser_struct_typed_params_and_members)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }\n",
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
                                "float dot(const Vec3 a, const Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
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
                                "void bump(Vec3 v) { v.x = v.x + 10.0; v.y = v.y + 20.0; }\n"
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
                                "extern float length_sq(const Vec3 v);\n"
                                "extern void scale_into(const Vec3 src, float s, Vec3 dst);\n"
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

#if STRATA_TEST_HAS_LLVM
#include "Codegen/LLVMAot.h"
#include "Codegen/LLVMModuleBuilder.h"

STRATA_TEST(aot_emits_struct_object)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(const Vec3 a, const Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BuiltModule bm = BuildLlvmModule(mod, &diag, &arena, false, NULL);
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
#endif

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
    StrataJit* jit = CompileJit("int identity(const int x) { return x; }\n"
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
    StrataJit* jit = CompileJit("float half_val(const float x) { return x * 0.5; }\n"
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
    StrataJit* jit = CompileJit("void consume(const int x) { int unused = x + 1; }\n"
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
    StrataJit* jit = CompileJit("int sum(const int a, const int b, const int c) { return a + b + c; }\n"
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
                                "float dot(const Vec3 a, const Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
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

STRATA_TEST(jit_struct_equality_is_memberwise)
{
    /* Struct `==` is member-wise: a whole-value byte compare where the
       layout allows (zero-initialized padding included — the Pad struct has
       7 padding bytes after `a`), recursion otherwise. */
    StrataJit* jit = CompileJit("struct Plain { int x; long y; bool f; };\n"
                                "struct Pad { byte a; long b; };\n"
                                "int entry() {\n"
                                "  Plain p1 = { .x = 1, .y = 2, .f = true };\n"
                                "  Plain p2 = { .x = 1, .y = 2, .f = true };\n"
                                "  Plain p3 = { .x = 1, .y = 3, .f = true };\n"
                                "  Pad d1 = { .a = 1, .b = 2 };\n"
                                "  Pad d2 = { .a = 1, .b = 2 };\n"
                                "  Pad d3 = { .a = 2, .b = 2 };\n"
                                "  int r = 0;\n"
                                "  if (p1 == p2) { r += 1; }\n"
                                "  if (p1 != p3) { r += 2; }\n"
                                "  if (d1 == d2) { r += 4; }\n"   /* equal only if padding is zeroed */
                                "  if (d1 != d3) { r += 8; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 15);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_equality_float_fields_are_ieee754)
{
    /* Structs with float fields can NEVER memcmp: IEEE-754 says -0.0 == 0.0
       (bytes differ) and NaN != NaN (bit-identical NaNs differ). */
    StrataJit* jit = CompileJit("struct V { float x; float y; };\n"
                                "int entry() {\n"
                                "  V a = { .x = 0.0, .y = 1.0 };\n"
                                "  V b = { .x = -0.0, .y = 1.0 };\n"   /* bytes differ, values equal */
                                "  float z = 0.0;\n"
                                "  V n1 = { .x = z / z, .y = 1.0 };\n" /* NaN, bit-identical below */
                                "  V n2 = { .x = z / z, .y = 1.0 };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"
                                "  if (n1 != n2) { r += 2; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_string_field_equality_is_content)
{
    /* Boxes auto-deref: `^Rec == ^Rec` compares the structs MEMBER-WISE —
       a string field compares by content (strata_str_eq), never by buffer
       pointer. Distinct cells with equal contents are equal. */
    StrataJit* jit = CompileJit("struct Rec { string name; int id; };\n"
                                "int entry() {\n"
                                "  ^Rec a = Rec { .name = \"hi\", .id = 1 };\n"
                                "  ^Rec b = Rec { .name = \"hi\", .id = 1 };\n"
                                "  ^Rec c = Rec { .name = \"hi\", .id = 2 };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"      /* deref: member-wise, content equal */
                                "  if (a != c) { r += 2; }\n"      /* id differs */
                                "  if (a.name == b.name) { r += 4; }\n"   /* field content equal across cells */
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 7);
        }
        strataJitDestroy(jit);
    }
}

/* Host helper for the distinct-buffers proof: extern string params cross as
   the fat's data pointer (char*), so this is a raw pointer equality on the
   string buffers themselves. */
static int SameStrDataPtr(const char* a, const char* b)
{
    return a == b;
}

STRATA_TEST(jit_struct_string_field_content_eq_distinct_buffers)
{
    /* Two struct instances with the same string VALUES but different
       buffers: the constructor heap-copies the literal per box, so the
       data pointers must differ — and the box `==` (which derefs and
       compares member-wise) must still see CONTENT equality. */
    StrataJit* jit = CompileJit("struct Rec { string name; int id; };\n"
                                "extern int samestrptr(string a, string b);\n"
                                "int entry() {\n"
                                "  ^Rec a = Rec { .name = \"hi\", .id = 1 };\n"
                                "  ^Rec b = Rec { .name = \"hi\", .id = 1 };\n"
                                "  int r = 0;\n"
                                "  if (samestrptr(a.name, b.name) == 0) { r += 1; }\n" /* buffers differ */
                                "  if (a == b) { r += 2; }\n"                 /* deref: content equal */
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        STRATA_CHECK_EQ(strataJitAddSymbol(jit, "samestrptr", (void*)&SameStrDataPtr), 1);

        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_boxed_struct_array_equality_derefs_each)
{
    /* `^Rec[]` compares element-wise, DEREFING each box: same string
       values in distinct cells compare equal. */
    StrataJit* jit = CompileJit("struct Rec { string name; int id; };\n"
                                "int entry() {\n"
                                "  ^Rec[] a = { Rec { .name = \"hi\", .id = 1 }, Rec { .name = \"yo\", .id = 2 } };\n"
                                "  ^Rec[] b = { Rec { .name = \"hi\", .id = 1 }, Rec { .name = \"yo\", .id = 2 } };\n"
                                "  ^Rec[] c = { Rec { .name = \"hi\", .id = 1 }, Rec { .name = \"no\", .id = 2 } };\n"
                                "  ^Rec[] d = { Rec { .name = \"hi\", .id = 1 } };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"      /* each cell deref'd: content equal */
                                "  if (a != c) { r += 2; }\n"      /* second element's string differs */
                                "  if (a != d) { r += 4; }\n"      /* length fast-out */
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 7);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_fixed_array_of_floats_equality)
{
    /* A fixed-array field with float elements forces the structural path:
       element-wise fcmp, where memcmp would disagree (-0.0 vs 0.0). */
    StrataJit* jit = CompileJit("struct F { float[3] vs; };\n"
                                "int entry() {\n"
                                "  F a = { .vs = {0.0, 1.0, 2.0} };\n"
                                "  F b = { .vs = {-0.0, 1.0, 2.0} };\n"   /* bytes differ, values equal */
                                "  F c = { .vs = {0.0, 1.0, 3.0} };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"
                                "  if (a != c) { r += 2; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_field_fixed_array_equality)
{
    /* Fixed-array FIELDS are equality-comparable expressions too: whole
       struct and the field itself compare element-wise/byte-wise. */
    StrataJit* jit = CompileJit("struct Buf { int[4] cells; };\n"
                                "int entry() {\n"
                                "  Buf a = { .cells = {1, 2, 3, 4} };\n"
                                "  Buf b = { .cells = {1, 2, 3, 4} };\n"
                                "  Buf c = { .cells = {1, 2, 4, 4} };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"
                                "  if (a != c) { r += 2; }\n"
                                "  if (a.cells == b.cells) { r += 4; }\n"
                                "  if (a.cells != c.cells) { r += 8; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 15);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_nested_struct_equality)
{
    /* Nested structs recurse member-wise; a float at any depth forces the
       structural path for the whole chain. */
    StrataJit* jit = CompileJit("struct Inner { int v; float w; };\n"
                                "struct Outer { Inner i; long tag; };\n"
                                "int entry() {\n"
                                "  Outer a = { .i = Inner { .v = 1, .w = -0.0 }, .tag = 7 };\n"
                                "  Outer b = { .i = Inner { .v = 1, .w = 0.0 }, .tag = 7 };\n"
                                "  Outer c = { .i = Inner { .v = 2, .w = 0.0 }, .tag = 7 };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"   /* -0.0 == 0.0 deep inside */
                                "  if (a != c) { r += 2; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_extern_struct_equality_is_memberwise)
{
    /* Extern (host-layout) structs never memcmp — the host may leave padding
       untouched — but they still compare member-wise. */
    StrataJit* jit = CompileJit("extern struct Point { int x; int y; };\n"
                                "int entry() {\n"
                                "  Point a = { .x = 1, .y = 2 };\n"
                                "  Point b = { .x = 1, .y = 2 };\n"
                                "  Point c = { .x = 1, .y = 3 };\n"
                                "  int r = 0;\n"
                                "  if (a == b) { r += 1; }\n"
                                "  if (a != c) { r += 2; }\n"
                                "  return r;\n"
                                "}\n");
    STRATA_CHECK(jit != NULL);
    if (jit)
    {
        int (*f)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
        STRATA_CHECK(f != NULL);
        if (f)
        {
            STRATA_CHECK_EQ(f(), 3);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(struct_equality_mixed_types_rejected)
{
    /* Distinct struct types, and structs against scalars, never compare. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct A { int x; };\n"
        "struct B { int x; };\n"
        "int entry() {\n"
        "  A a = { .x = 1 };\n"
        "  B b = { .x = 1 };\n"
        "  if (a == b) { return 1; }\n"
        "  if (a == 1) { return 2; }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "invalid operands to binary operator ('A' and 'B')") != NULL);
    STRATA_CHECK(strstr(d, "invalid operands to binary operator ('A' and 'int')") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(struct_equality_ordering_rejected)
{
    /* Only ==/!= exist for structs; ordering is a compile error. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseAndResolve(
        "struct A { int x; };\n"
        "int entry() {\n"
        "  A a = { .x = 1 };\n"
        "  A b = { .x = 2 };\n"
        "  if (a < b) { return 1; }\n"
        "  return 0;\n"
        "}\n",
        &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    SourceManager sm; SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "invalid operands to binary operator ('A' and 'A')") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

