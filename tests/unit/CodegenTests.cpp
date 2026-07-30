// Code generation unit tests.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/Codegen/CodegenBackend.h"

#if defined(STRATA_ENABLE_LLVM)
#include "strata/Codegen/LLVMCApi.h"
#endif

#include <string>

using namespace strata;
using namespace strata::test_util;

namespace {
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

CodegenResult genText(std::string_view src) {
    DiagnosticEngine diag;
    auto mod = parseModule(src, diag);
    return createTextBackend()->generate(*mod);
}
} // namespace

STRATA_TEST(text_emits_function_signature) {
    auto res = genText("int f() { return 7; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "define i32 @f"));
    STRATA_CHECK(contains(res.output, "ret i32 7"));
}

STRATA_TEST(text_emits_arithmetic) {
    auto res = genText("int add(int a, int b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "= add i32"));
}

STRATA_TEST(text_emits_float_arithmetic) {
    auto res = genText("float f(float a, float b) { return a + b; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "define float @f"));
    STRATA_CHECK(contains(res.output, "= fadd float"));
}

STRATA_TEST(text_emits_control_flow) {
    auto res = genText(
        "int g(int n) { int x = 0; while (x < n) { x = x + 1; } return x; }");
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "br label"));
    STRATA_CHECK(contains(res.output, "icmp slt"));
}

STRATA_TEST(text_forward_call_resolves) {
    auto res = genText(
        "int main() { return f(); } int f() { return 1; }");
    STRATA_CHECK(res.ok);
    // 'main' calls 'f' before it is defined; the IR contains the call and the
    // later definition, which LLVM resolves via forward references.
    STRATA_CHECK(contains(res.output, "= call i32 @f"));
    STRATA_CHECK(contains(res.output, "define i32 @f"));
    // Functions are never both declared and defined.
    STRATA_CHECK(!contains(res.output, "declare i32 @f"));
}

#if defined(STRATA_ENABLE_LLVM)
STRATA_TEST(llvm_backend_builds_module) {
    DiagnosticEngine diag;
    auto mod = parseModule("int add(int a, int b) { return a + b; }", diag);
    auto backend = createLLVMBackend();
    STRATA_CHECK(backend != nullptr);
    auto res = backend->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "define"));
    STRATA_CHECK(contains(res.output, "add"));
    // A clean build reports no verifier warnings.
    STRATA_CHECK(!contains(res.output, "VERIFY WARNING"));
}

STRATA_TEST(llvm_reports_version) {
    unsigned maj = 0, min = 0, pat = 0;
    LLVMGetVersion(&maj, &min, &pat);
    STRATA_CHECK(maj > 0);
}
#endif
