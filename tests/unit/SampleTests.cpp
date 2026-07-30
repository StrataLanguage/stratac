// End-to-end test: compile the bundled sample program through the library.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/Codegen/CodegenBackend.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string loadSample() {
    const char* dir = STRATA_SAMPLE_DIR; // configured by CMake
    std::string path = std::string(dir) + "/hello.strata";
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

STRATA_TEST(sample_compiles_to_text_ir) {
    std::string src = loadSample();
    STRATA_CHECK(!src.empty());

    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(src, diag, "hello.strata");
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(mod->functions.size() >= 5);

    auto res = strata::createTextBackend()->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(std::strstr(res.output.c_str(), "define i32 @add") != nullptr);
    STRATA_CHECK(std::strstr(res.output.c_str(), "br label") != nullptr); // control flow present
}
