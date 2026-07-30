// JIT + AOT execution tests: actually run compiled Strata code.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/strata.h"

#include <cstdio>
#include <cstdint>

#if defined(STRATA_ENABLE_LLVM)
#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/LLVMAot.h"

#include <fstream>

STRATA_TEST(jit_runs_int_addition) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(
        c, "int add(int a, int b) { return a + b; }", "math", &err);
    if (!jit) {
        std::printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree(const_cast<char*>(err));
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }
    STRATA_CHECK(jit != nullptr);

    auto add = reinterpret_cast<int (*)(int, int)>(strataJitGetFunction(jit, "add"));
    STRATA_CHECK(add != nullptr);
    if (add) {
        STRATA_CHECK_EQ(add(2, 3), 5);
        STRATA_CHECK_EQ(add(-1, 1), 0);
        STRATA_CHECK_EQ(add(100, 23), 123);
    }
    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_noarg_function_and_calls) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(
        c,
        "int sq(int x) { return x * x; }\n"
        "int answer() { return sq(7); }\n",
        "calls", &err);
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto answer = reinterpret_cast<int (*)()>(strataJitGetFunction(jit, "answer"));
        STRATA_CHECK(answer != nullptr);
        if (answer) STRATA_CHECK_EQ(answer(), 49);

        auto missing = strataJitGetFunction(jit, "does_not_exist");
        STRATA_CHECK(missing == nullptr);

        strataJitDestroy(jit);
    } else {
        strataFree(const_cast<char*>(err));
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_runs_float_function) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(
        c, "float twice(float x) { return x * 2.0; }", "flt", &err);
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto twice = reinterpret_cast<float (*)(float)>(strataJitGetFunction(jit, "twice"));
        STRATA_CHECK(twice != nullptr);
        if (twice) {
            float r = twice(21.0f);
            STRATA_CHECK(r > 41.999f && r < 42.001f);
        }
        strataJitDestroy(jit);
    } else {
        strataFree(const_cast<char*>(err));
    }
    strataCompilerDestroy(c);
}

STRATA_TEST(aot_emits_native_object_file) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(
        "int add(int a, int b) { return a + b; }", diag);
    STRATA_CHECK(!diag.hasErrors());

    std::string notes, err;
    strata::BuiltModule bm = strata::buildLLVMModule(*mod, notes);
    std::string path = "strata_aot_test.o";
    bool ok = strata::emitNativeFile(bm, path, /*assembly=*/false, err);
    if (!ok) std::printf("  AOT emission failed: %s\n", err.c_str());
    STRATA_CHECK(ok);

    std::ifstream in(path, std::ios::binary);
    STRATA_CHECK(in.good());
    in.seekg(0, std::ios::end);
    STRATA_CHECK(in.tellg() > 0);
}

STRATA_TEST(aot_emits_assembly_file) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(
        "int forty_two() { return 42; }", diag);
    STRATA_CHECK(!diag.hasErrors());

    std::string notes, err;
    strata::BuiltModule bm = strata::buildLLVMModule(*mod, notes);
    std::string path = "strata_aot_test.s";
    bool ok = strata::emitNativeFile(bm, path, /*assembly=*/true, err);
    if (!ok) std::printf("  AOT asm emission failed: %s\n", err.c_str());
    STRATA_CHECK(ok);

    std::ifstream in(path, std::ios::binary);
    STRATA_CHECK(in.good());
    in.seekg(0, std::ios::end);
    STRATA_CHECK(in.tellg() > 0);
}

// ---- extern: Strata calls into host-provided functions ----

// Host functions the engine would register. Plain C ABI; the symbol names match
// the Strata `extern` declarations.
static int host_double(int x) { return x * 2; }
static int host_add(int a, int b) { return a + b; }

STRATA_TEST(jit_calls_host_extern_function) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(
        c,
        "extern int host_double(int x);\n"
        "extern int host_add(int a, int b);\n"
        "int entry(int x) { return host_add(host_double(x), 1); }\n",
        "ext", &err);
    STRATA_CHECK(jit != nullptr);
    if (!jit) {
        strataFree(const_cast<char*>(err));
        strataCompilerDestroy(c);
        return;
    }

    // The host can discover what the script needs.
    STRATA_CHECK_EQ(strataJitGetExternSymbolCount(jit), (size_t)2);

    // Bind host functions before resolving any Strata function (which compiles).
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_double", (void*)&host_double), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "host_add", (void*)&host_add), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "not_declared", (void*)&host_add), 0);

    auto entry = reinterpret_cast<int (*)(int)>(strataJitGetFunction(jit, "entry"));
    STRATA_CHECK(entry != nullptr);
    if (entry) {
        // entry(5) == host_add(host_double(5), 1) == host_add(10, 1) == 11
        STRATA_CHECK_EQ(entry(5), 11);
        STRATA_CHECK_EQ(entry(0), 1);
    }
    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

#endif // STRATA_ENABLE_LLVM
