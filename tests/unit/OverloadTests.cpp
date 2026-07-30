// Overload resolution tests.
#include "Util.hpp"
#include "strata/Codegen/CodegenBackend.h"
#include "strata/Sema/ResolveOverloads.h"
#include "strata/Test.hpp"

#include <string>

#ifdef STRATA_ENABLE_LLVM
#include "strata/strata.h"
#endif

using namespace strata;
using namespace strata::test_util;

namespace
{
bool Contains(const std::string& h, const std::string& n)
{
    return h.find(n) != std::string::npos;
}

// Parse + run overload resolution; returns the module.
std::unique_ptr<Module> Resolve(std::string_view src, DiagnosticEngine& diag)
{
    return ParseAndResolve(src, diag);
}
} // namespace

STRATA_TEST(single_function_keeps_base_name)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int only(int a) { return a; }\nint entry() { return only(5); }\n", diag);
    STRATA_CHECK(!diag.HasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "only");
    STRATA_CHECK(mod->functions[1]->mangledName == "entry");
}

STRATA_TEST(overloads_get_mangled_names_and_resolve)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int add(int a, int b) { return a + b; }\n"
                       "float add(float a, float b) { return a + b; }\n"
                       "int entry_i() { return add(2, 3); }\n"        // -> int overload
                       "float entry_f() { return add(2.0, 3.0); }\n", // -> float overload
                       diag);
    STRATA_CHECK(!diag.HasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "add$int$int");
    STRATA_CHECK(mod->functions[1]->mangledName == "add$float$float");

    // The calls were rewritten to the chosen mangled symbol.
    auto& callsI = static_cast<Block*>(mod->functions[2]->body.get())->statements;
    auto* retI = static_cast<ReturnStmt*>(callsI.front().get());
    STRATA_CHECK(static_cast<CallExpr*>(retI->value.get())->callee == "add$int$int");
    auto& callsF = static_cast<Block*>(mod->functions[3]->body.get())->statements;
    auto* retF = static_cast<ReturnStmt*>(callsF.front().get());
    STRATA_CHECK(static_cast<CallExpr*>(retF->value.get())->callee == "add$float$float");
}

STRATA_TEST(text_emits_distinct_overload_symbols)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int add(int a, int b) { return a + b; }\n"
                       "float add(float a, float b) { return a + b; }\n"
                       "int entry_i() { return add(2, 3); }\n"
                       "float entry_f() { return add(2.0, 3.0); }\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
    auto res = CreateTextBackend()->Generate(*mod);
    STRATA_CHECK(res.ok);
    STRATA_CHECK(Contains(res.output, "define i32 @add$int$int"));
    STRATA_CHECK(Contains(res.output, "define float @add$float$float"));
    STRATA_CHECK(Contains(res.output, "call i32 @add$int$int"));
    STRATA_CHECK(Contains(res.output, "call float @add$float$float"));
}

STRATA_TEST(struct_vs_scalar_overload_resolves)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct Vec3 { float x; float y; float z; };\n"
                       "float mag(in Vec3 v) { return v.x + v.y + v.z; }\n"
                       "float mag(float s) { return s; }\n"
                       "float ev() { Vec3 v; v.x = 1.0; v.y = 2.0; v.z = 3.0; return mag(v); }\n"
                       "float es() { return mag(10.0); }\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
    STRATA_CHECK(mod->functions[0]->mangledName == "mag$Vec3"); // struct is exact match
    STRATA_CHECK(mod->functions[1]->mangledName == "mag$float");
}

STRATA_TEST(ambiguous_overload_is_an_error)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int f(int a, float b) { return 1; }\n"
                       "int f(float a, int b) { return 2; }\n"
                       "int entry() { return f(1, 2); }\n",
                       diag); // both score 1 -> ambiguous
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(no_matching_overload_is_an_error)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "int f(int a) { return a; }\n"
                       "int entry() { V v; return f(v); }\n",
                       diag); // V is not int-convertible
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(undefined_function_is_an_error)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int entry() { return foofdofdofd(); }\n", diag);
    STRATA_CHECK(diag.HasErrors());
    std::string d = diag.Format(SourceManager{});
    STRATA_CHECK(Contains(d, "unknown function 'foofdofdofd'"));
}

STRATA_TEST(constructor_call_is_not_unknown)
{
    // Vec3(1,2,3) is a constructor, not a function call -- must not trigger
    // the "unknown function" diagnostic.
    DiagnosticEngine diag;
    auto mod = Resolve("struct Vec3 { float x; float y; float z; };\n"
                       "int entry() { Vec3 v = Vec3(1, 2, 3); return 0; }\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
}

STRATA_TEST(extern_struct_param_requires_direction)
{
    // A bare struct param (extern or not) must declare in/out/inout -- structs
    // are always passed by reference.
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "extern void take(V v);\n",
                       diag);
    STRATA_CHECK(diag.HasErrors());
    auto ok = Resolve("struct V { float x; };\n"
                      "extern void take(in V v);\n",
                      diag);
    (void)ok;
}

STRATA_TEST(any_struct_param_requires_direction)
{
    // The rule is not extern-specific: a plain Strata function too.
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "void take(V v) { };\n",
                       diag);
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(struct_inout_param_is_allowed)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "void bump(inout V v) { v.x = v.x + 1.0; }\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
}

STRATA_TEST(extern_struct_param_with_in_is_ok)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "extern void take(in V v);\n"
                       "extern void fill(out V v);\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
}

STRATA_TEST(extern_cannot_return_struct_by_value)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct V { float x; };\n"
                       "extern V make();\n",
                       diag);
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(handle_param_does_not_need_direction)
{
    // Handles are pointer-sized and passed by value; they need no in/out/inout.
    DiagnosticEngine diag;
    auto mod = Resolve("handle Entity;\n"
                       "extern int id_of(Entity e);\n",
                       diag);
    STRATA_CHECK(!diag.HasErrors());
}

STRATA_TEST(handle_cannot_have_members_accessed)
{
    DiagnosticEngine diag;
    auto mod = Resolve("handle Entity;\n"
                       "extern Entity make();\n"
                       "float entry() { Entity e = make(); return e.x; }\n",
                       diag); // e.x on a handle
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(in_scalar_param_is_const)
{
    DiagnosticEngine diag;
    auto mod = Resolve("void foo(in int x) { x = 5; }\n", diag);
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(in_scalar_param_compound_assign_is_error)
{
    DiagnosticEngine diag;
    auto mod = Resolve("void foo(in int x) { x += 5; }\n", diag);
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(in_struct_param_member_is_const)
{
    DiagnosticEngine diag;
    auto mod = Resolve("struct Vec3 { float x; float y; float z; };\n"
                       "void foo(in Vec3 v) { v.y = 3.0; }\n",
                       diag);
    STRATA_CHECK(diag.HasErrors());
}

STRATA_TEST(in_param_can_be_read)
{
    DiagnosticEngine diag;
    auto mod = Resolve("int foo(in int x) { return x + 1; }\n", diag);
    STRATA_CHECK(!diag.HasErrors());
}

#ifdef STRATA_ENABLE_LLVM
STRATA_TEST(jit_runs_resolved_overloads)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(c,
                                            "int add(int a, int b) { return a + b; }\n"
                                            "float add(float a, float b) { return a + b; }\n"
                                            "int entry_i() { return add(20, 3); }\n"        // -> int overload -> 23
                                            "float entry_f() { return add(20.0, 3.0); }\n", // -> float overload -> 23.0
                                            "ovl", &err);
    STRATA_CHECK(jit != nullptr);
    if (!jit)
    {
        strataFree(const_cast<char*>(err));
        strataCompilerDestroy(c);
        return;
    }

    auto ei = reinterpret_cast<int (*)()>(strataJitGetFunction(jit, "entry_i"));
    auto ef = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry_f"));
    STRATA_CHECK(ei != nullptr);
    STRATA_CHECK(ef != nullptr);
    if (ei)
    {
        STRATA_CHECK_EQ(ei(), 23);
    }

    if (ef)
    {
        float r = ef();
        STRATA_CHECK(r > 22.9f && r < 23.1f);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

#endif
