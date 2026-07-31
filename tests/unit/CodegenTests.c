#include "Util.h"
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Test.h"

#include "strata/Codegen/LLVMCApi.h"

#include <string.h>

static bool Contains(const char* hay, const char* needle)
{
    return strstr(hay, needle) != NULL;
}


static CodegenResult GenLlvm(const char* src)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(src, &diag, &arena);
    CodegenResult res = GenerateLlvmIr(mod);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
    return res;
}

STRATA_TEST(llvm_emits_function_signature)
{
    CodegenResult res = GenLlvm("int f() { return 7; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @f"));
    STRATA_CHECK(Contains(res.output, "ret i32 7"));
}

STRATA_TEST(llvm_emits_arithmetic)
{
    CodegenResult res = GenLlvm("int add(int a, int b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "= add i32"));
}

STRATA_TEST(llvm_emits_float_arithmetic)
{
    CodegenResult res = GenLlvm("float f(float a, float b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define float @f"));
    STRATA_CHECK(Contains(res.output, "= fadd float"));
}

STRATA_TEST(llvm_emits_control_flow)
{
    CodegenResult res = GenLlvm("int g(int n) { int x = 0; while (x < n) { x = x + 1; } return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "br label"));
    STRATA_CHECK(Contains(res.output, "icmp slt"));
}

STRATA_TEST(llvm_forward_call_resolves)
{
    CodegenResult res = GenLlvm("int main() { return f(); } int f() { return 1; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "call i32 @f"));
    STRATA_CHECK(Contains(res.output, "define i32 @f"));
    STRATA_CHECK(!Contains(res.output, "declare i32 @f"));
}

STRATA_TEST(llvm_in_scalar_param_is_by_reference)
{
    CodegenResult res = GenLlvm("int foo(in int x) { return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @foo(ptr"));
    STRATA_CHECK(Contains(res.output, "load i32, ptr"));
}

STRATA_TEST(llvm_plain_scalar_param_is_by_value)
{
    CodegenResult res = GenLlvm("int foo(int x) { return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @foo(i32"));
    STRATA_CHECK(!Contains(res.output, "define i32 @foo(ptr"));
}

STRATA_TEST(llvm_in_scalar_call_site_passes_address)
{
    CodegenResult res = GenLlvm("int foo(in int x) { return x; }\n"
                                "int entry() { int v = 5; return foo(v); }\n");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "call i32 @foo(ptr"));
}

STRATA_TEST(llvm_in_float_param_is_by_reference)
{
    CodegenResult res = GenLlvm("float foo(in float x) { return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define float @foo(ptr"));
    STRATA_CHECK(Contains(res.output, "load float, ptr"));
}

STRATA_TEST(llvm_inout_and_out_remain_by_reference)
{
    CodegenResult res = GenLlvm("void foo(out int x) { x = 1; }\n"
                                "void bar(inout int y) { y = y + 1; }\n");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define void @foo(ptr"));
    STRATA_CHECK(Contains(res.output, "define void @bar(ptr"));
}

STRATA_TEST(llvm_backend_builds_module)
{
    CodegenResult res = GenLlvm("int add(int a, int b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define"));
    STRATA_CHECK(Contains(res.output, "add"));
    STRATA_CHECK(!Contains(res.output, "VERIFY WARNING"));
}

STRATA_TEST(llvm_reports_version)
{
    unsigned maj = 0;
    unsigned min = 0;
    unsigned pat = 0;
    LLVMGetVersion(&maj, &min, &pat);
    STRATA_CHECK(maj > 0);
}

STRATA_TEST(llvm_overloaded_in_struct_param)
{
    CodegenResult res = GenLlvm("struct Vec3 { float x; float y; float z; };\n"
                                "float foo(float a, float b) { return a; }\n"
                                "float foo(in Vec3 v) { return foo(v.x, v.y); }\n");
    STRATA_CHECK(res.ok);
}

