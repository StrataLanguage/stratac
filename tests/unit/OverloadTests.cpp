// Overload resolution tests.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/Sema/ResolveOverloads.h"
#include "strata/Codegen/CodegenBackend.h"

#include <string>

#if defined(STRATA_ENABLE_LLVM)
#include "strata/strata.h"
#endif

using namespace strata;
using namespace strata::test_util;

namespace {
bool contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// Parse + run overload resolution; returns the module.
std::unique_ptr<Module> resolve(std::string_view src, DiagnosticEngine& diag) {
    auto mod = parseModule(src, diag);
    if (mod) resolveOverloads(*mod, diag);
    return mod;
}
} // namespace

STRATA_TEST(single_function_keeps_base_name) {
    DiagnosticEngine diag;
    auto mod = resolve("int only(int a) { return a; }\nint entry() { return only(5); }\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "only");
    STRATA_CHECK(mod->functions[1]->mangledName == "entry");
}

STRATA_TEST(overloads_get_mangled_names_and_resolve) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n"
        "int entry_i() { return add(2, 3); }\n"      // -> int overload
        "float entry_f() { return add(2.0, 3.0); }\n", // -> float overload
        diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "add$int$int");
    STRATA_CHECK(mod->functions[1]->mangledName == "add$float$float");

    // The calls were rewritten to the chosen mangled symbol.
    auto& calls_i = static_cast<Block*>(mod->functions[2]->body.get())->statements;
    auto ret_i = static_cast<ReturnStmt*>(calls_i.front().get());
    STRATA_CHECK(static_cast<CallExpr*>(ret_i->value.get())->callee == "add$int$int");
    auto& calls_f = static_cast<Block*>(mod->functions[3]->body.get())->statements;
    auto ret_f = static_cast<ReturnStmt*>(calls_f.front().get());
    STRATA_CHECK(static_cast<CallExpr*>(ret_f->value.get())->callee == "add$float$float");
}

STRATA_TEST(text_emits_distinct_overload_symbols) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n"
        "int entry_i() { return add(2, 3); }\n"
        "float entry_f() { return add(2.0, 3.0); }\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    auto res = createTextBackend()->generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(contains(res.output, "define i32 @add$int$int"));
    STRATA_CHECK(contains(res.output, "define float @add$float$float"));
    STRATA_CHECK(contains(res.output, "call i32 @add$int$int"));
    STRATA_CHECK(contains(res.output, "call float @add$float$float"));
}

STRATA_TEST(struct_vs_scalar_overload_resolves) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "struct Vec3 { float x; float y; float z; };\n"
        "float mag(Vec3 v) { return v.x + v.y + v.z; }\n"
        "float mag(float s) { return s; }\n"
        "float ev() { Vec3 v; v.x = 1.0; v.y = 2.0; v.z = 3.0; return mag(v); }\n"
        "float es() { return mag(10.0); }\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "mag$Vec3");   // struct is exact match
    STRATA_CHECK(mod->functions[1]->mangledName == "mag$float");
}

STRATA_TEST(ambiguous_overload_is_an_error) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "int f(int a, float b) { return 1; }\n"
        "int f(float a, int b) { return 2; }\n"
        "int entry() { return f(1, 2); }\n", diag); // both score 1 -> ambiguous
    STRATA_CHECK(diag.hasErrors());
}

STRATA_TEST(no_matching_overload_is_an_error) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "struct V { float x; };\n"
        "int f(int a) { return a; }\n"
        "int entry() { V v; return f(v); }\n", diag); // V is not int-convertible
    STRATA_CHECK(diag.hasErrors());
}

STRATA_TEST(extern_struct_param_requires_direction) {
    // A bare struct param on an extern must declare in/out/inout (it crosses
    // the host boundary by pointer).
    DiagnosticEngine diag;
    auto mod = resolve(
        "struct V { float x; };\n"
        "extern void take(V v);\n", diag);
    STRATA_CHECK(diag.hasErrors());
    auto ok = resolve(
        "struct V { float x; };\n"
        "extern void take(in V v);\n", diag);
    (void)ok;
}

STRATA_TEST(extern_struct_param_with_in_is_ok) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "struct V { float x; };\n"
        "extern void take(in V v);\n"
        "extern void fill(out V v);\n", diag);
    STRATA_CHECK(!diag.hasErrors());
}

STRATA_TEST(extern_cannot_return_struct_by_value) {
    DiagnosticEngine diag;
    auto mod = resolve(
        "struct V { float x; };\n"
        "extern V make();\n", diag);
    STRATA_CHECK(diag.hasErrors());
}

STRATA_TEST(opaque_handle_extern_does_not_need_direction) {
    // Opaque handles are already pointer-sized, so they're fine without a mod.
    DiagnosticEngine diag;
    auto mod = resolve(
        "extern struct Entity;\n"
        "extern int id_of(Entity e);\n", diag);
    STRATA_CHECK(!diag.hasErrors());
}

#if defined(STRATA_ENABLE_LLVM)
STRATA_TEST(jit_runs_resolved_overloads) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(
        c,
        "int add(int a, int b) { return a + b; }\n"
        "float add(float a, float b) { return a + b; }\n"
        "int entry_i() { return add(20, 3); }\n"        // -> int overload -> 23
        "float entry_f() { return add(20.0, 3.0); }\n", // -> float overload -> 23.0
        "ovl", &err);
    STRATA_CHECK(jit != nullptr);
    if (!jit) { strataFree(const_cast<char*>(err)); strataCompilerDestroy(c); return; }

    auto ei = reinterpret_cast<int (*)()>(strataJitGetFunction(jit, "entry_i"));
    auto ef = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry_f"));
    STRATA_CHECK(ei != nullptr);
    STRATA_CHECK(ef != nullptr);
    if (ei) STRATA_CHECK_EQ(ei(), 23);
    if (ef) { float r = ef(); STRATA_CHECK(r > 22.9f && r < 23.1f); }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
#endif
