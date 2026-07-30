// End-to-end tests: compile the bundled sample programs through the library.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/Codegen/CodegenBackend.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string loadSample(const char* name) {
    const char* dir = STRATA_SAMPLE_DIR; // configured by CMake
    std::string path = std::string(dir) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

STRATA_TEST(sample_hello_compiles_to_text_ir) {
    std::string src = loadSample("hello.strata");
    STRATA_CHECK(!src.empty());

    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(src, diag, "hello.strata");
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(mod->functions.size() >= 3); // add, mul, main

    auto res = strata::createTextBackend()->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(std::strstr(res.output.c_str(), "define i32 @add") != nullptr);
    STRATA_CHECK(std::strstr(res.output.c_str(), "call i32") != nullptr); // main calls add/mul
}

#if defined(STRATA_ENABLE_LLVM)
STRATA_TEST(sample_structs_compile_with_native_backend) {
    std::string src = loadSample("structs.strata");
    STRATA_CHECK(!src.empty());

    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(src, diag, "structs.strata");
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(!mod->structs.empty());

    auto res = strata::createLLVMBackend()->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(std::strstr(res.output.c_str(), "%struct.Vec3") != nullptr);
    STRATA_CHECK(std::strstr(res.output.c_str(), "%struct.Particle") != nullptr);
}
#endif

STRATA_TEST(sample_control_flow_lowers_in_text_backend) {
    std::string src = loadSample("control_flow.strata");
    STRATA_CHECK(!src.empty());

    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(src, diag, "control_flow.strata");
    STRATA_CHECK(!diag.hasErrors());

    auto res = strata::createTextBackend()->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(std::strstr(res.output.c_str(), "br label") != nullptr);     // loops/branches
    STRATA_CHECK(std::strstr(res.output.c_str(), "call i32 @fibonacci") != nullptr); // recursion
}
