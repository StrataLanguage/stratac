#include "Util.h"
#include "Codegen/CodegenBackend.h"
#include "Test.h"

#include "Codegen/LLVMCApi.h"

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
    CodegenResult res = GenLlvm("int foo(ref int x) { return x; }");
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
    CodegenResult res = GenLlvm("int foo(ref int x) { return x; }\n"
                                "int entry() { int v = 5; return foo(v); }\n");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "call i32 @foo(ptr"));
}

STRATA_TEST(llvm_in_float_param_is_by_reference)
{
    CodegenResult res = GenLlvm("float foo(ref float x) { return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define float @foo(ptr"));
    STRATA_CHECK(Contains(res.output, "load float, ptr"));
}

STRATA_TEST(llvm_inout_and_out_remain_by_reference)
{
    CodegenResult res = GenLlvm("void foo(ref int x) { x = 1; }\n"
                                "void bar(ref int y) { y = y + 1; }\n");
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
                                "float foo(const Vec3 v) { return foo(v.x, v.y); }\n");
    STRATA_CHECK(res.ok);
}

/* Scalar pseudo-properties emit the exact C limit-macro bit patterns:
   FLT_MAX/FLT_MIN and DBL_MAX/DBL_MIN (hex-float IR spelling). */
STRATA_TEST(llvm_scalar_pseudo_constants_emit_correct_bits)
{
    CodegenResult res = GenLlvm("int f() {\n"
                                "  float fmax = float.max;\n"
                                "  float fmin = float.min;\n"
                                "  double dmax = double.max;\n"
                                "  double dmin = double.min;\n"
                                "  int x = 0;\n"
                                "  return x;\n"
                                "}\n");
    STRATA_CHECK(res.ok);
    /* The exact IR spelling of hex float constants varies across LLVM
       versions: newer LLVM prints the IEEE form ("f0x7F7FFFFF"), older
       builds print the double-widened encoding ("0x47EFFFFFE0000000") and
       may omit leading zeros (0x10000000000000). Accept any spelling of the
       limit-macro bit patterns. */
    STRATA_CHECK(Contains(res.output, "store float f0x7F7FFFFF") ||             /* FLT_MAX */
                 Contains(res.output, "store float 0x47EFFFFFE0000000"));
    STRATA_CHECK(Contains(res.output, "store float f0x00800000") ||             /* FLT_MIN */
                 Contains(res.output, "store float 0x3810000000000000"));
    STRATA_CHECK(Contains(res.output, "store double f0x7FEFFFFFFFFFFFFF") ||    /* DBL_MAX */
                 Contains(res.output, "store double 0x7FEFFFFFFFFFFFFF"));
    STRATA_CHECK(Contains(res.output, "store double f0x0010000000000000") ||    /* DBL_MIN */
                 Contains(res.output, "store double 0x0010000000000000") ||
                 Contains(res.output, "store double 0x10000000000000"));
}

/* A float literal assigned to a double local must be fpext'd, not stored as
   a 4-byte float into the 8-byte slot (that left the upper half as garbage
   and every double read as junk). */
STRATA_TEST(llvm_double_local_init_widens_float_literal)
{
    CodegenResult res = GenLlvm("double f() { double d = 2.5; return d; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "store double 2.500000e+00")); /* 2.5, full-width */
    STRATA_CHECK(!Contains(res.output, "store float"));
}

/* Mixing a float literal with a double operand must compute in the wider
   type (fpext the float side) - not emit a mismatched `fmul float X, double
   Y`, which corrupted the result. */
STRATA_TEST(llvm_mixed_float_double_arithmetic_widens)
{
    CodegenResult res = GenLlvm("double g = 3.0;\n"
                                "double f() { return g * 2.0; }\n");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "fmul double"));
    STRATA_CHECK(!Contains(res.output, "fmul float"));
}

/* A float literal passed to a double extern param must be fpext'd at the
   call site - not handed across at float width (a mismatched ABI). */
STRATA_TEST(llvm_float_literal_to_double_param_fpext)
{
    CodegenResult res = GenLlvm("extern double cos(double x);\n"
                                "double f() { return cos(1.5); }\n");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "call double @cos(double 1.500000e+00"));
}


