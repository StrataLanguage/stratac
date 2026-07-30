// Code generation unit tests.
#include "Util.hpp"
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Test.hpp"

#ifdef STRATA_ENABLE_LLVM
#include "strata/Codegen/LLVMCApi.h"
#endif

#include <string>

using namespace strata;
using namespace strata::test_util;

namespace
{
bool Contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

CodegenResult GenText(std::string_view src)
{
    DiagnosticEngine diag;
    auto mod = ParseModule(src, diag);
    return CreateTextBackend()->Generate(*mod);
}
} // namespace

STRATA_TEST(text_emits_function_signature)
{
    auto res = GenText("int f() { return 7; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @f"));
    STRATA_CHECK(Contains(res.output, "ret i32 7"));
}

STRATA_TEST(text_emits_arithmetic)
{
    auto res = GenText("int add(int a, int b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "= add i32"));
}

STRATA_TEST(text_emits_float_arithmetic)
{
    auto res = GenText("float f(float a, float b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define float @f"));
    STRATA_CHECK(Contains(res.output, "= fadd float"));
}

STRATA_TEST(text_emits_control_flow)
{
    auto res = GenText("int g(int n) { int x = 0; while (x < n) { x = x + 1; } return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "br label"));
    STRATA_CHECK(Contains(res.output, "icmp slt"));
}

STRATA_TEST(text_forward_call_resolves)
{
    auto res = GenText("int main() { return f(); } int f() { return 1; }");
    STRATA_CHECK(res.ok);
    // 'main' calls 'f' before it is defined; the IR contains the call and the
    // later definition, which LLVM resolves via forward references.
    STRATA_CHECK(Contains(res.output, "= call i32 @f"));
    STRATA_CHECK(Contains(res.output, "define i32 @f"));
    // Functions are never both declared and defined.
    STRATA_CHECK(!Contains(res.output, "declare i32 @f"));
}

#ifdef STRATA_ENABLE_LLVM
STRATA_TEST(llvm_backend_builds_module)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int add(int a, int b) { return a + b; }", diag);
    auto backend = CreateLlvmBackend();
    STRATA_CHECK(backend != nullptr);
    auto res = backend->Generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define"));
    STRATA_CHECK(Contains(res.output, "add"));
    // A clean build reports no verifier warnings.
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
#endif
