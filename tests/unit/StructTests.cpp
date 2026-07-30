// Struct + opaque-handle tests: user-defined and engine-provided types.
#include "strata/Test.hpp"
#include "Util.hpp"
#include "strata/strata.h"

#include <cstdio>
#include <cstdint>

// ---- Parsing (no codegen) ----

STRATA_TEST(parser_struct_declaration) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(
        "struct Vec3 { float x; float y; float z; };\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK_EQ(mod->structs.size(), (std::size_t)1);
    auto& s = mod->structs.front();
    STRATA_CHECK(s->name == "Vec3");
    STRATA_CHECK_EQ(s->fields.size(), (std::size_t)3);
    STRATA_CHECK(s->fields[0].name == "x");
    STRATA_CHECK(s->fields[0].type.name == "float");
}

STRATA_TEST(parser_opaque_struct) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule("handle Entity;\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK_EQ(mod->handles.size(), (std::size_t)1);
    STRATA_CHECK(mod->handles.front()->name == "Entity");
}

STRATA_TEST(parser_struct_typed_params_and_members) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(in Vec3 a, in Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }\n",
        diag);
    STRATA_CHECK(!diag.hasErrors());
    STRATA_CHECK_EQ(mod->functions.size(), (std::size_t)1);
    STRATA_CHECK(mod->functions[0]->params[0]->type.name == "Vec3");
}

// ---- Native execution (LLVM) ----
//
// Structs are value types *within* Strata: member access, positional
// construction, and by-value passing between Strata functions all live entirely
// in JIT-compiled code (same calling convention on both sides). The host
// observes results through scalar entry points. (Passing aggregates directly
// across the host<->JIT boundary is ABI-sensitive on Windows; engine APIs that
// cross that boundary should use opaque handles or scalars -- see
// jit_opaque_engine_handle.)
#if defined(STRATA_ENABLE_LLVM)

static StrataJit* compileJit(const char* src) {
    StrataCompiler* c = strataCompilerCreate();
    const char* err = nullptr;
    StrataJit* jit = strataJitCompileString(c, src, "structs", &err);
    if (err) strataFree(const_cast<char*>(err));
    // NOTE: leaks the compiler for the test's lifetime; the JIT keeps no
    // reference to it after compile, so this is harmless in a test.
    return jit;
}

STRATA_TEST(jit_struct_member_read_write) {
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "float entry() {\n"
        "  Vec3 v;\n"
        "  v.x = 1.0;\n"
        "  v.y = 2.0;\n"
        "  v.z = 3.0;\n"
        "  return v.x + v.y + v.z;\n"
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f(); // 1 + 2 + 3 = 6
            STRATA_CHECK(r > 5.9f && r < 6.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_constructor_and_return) {
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "Vec3 make(float a, float b, float c) { return Vec3(a, b, c); }\n"
        "float entry() {\n"
        "  Vec3 v = make(10.0, 20.0, 30.0);\n"
        "  return v.x + v.y + v.z;\n"
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f(); // 10 + 20 + 30 = 60
            STRATA_CHECK(r > 59.9f && r < 60.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_passed_between_strata_functions) {
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n"
        "float entry() {\n"
        "  Vec3 a; a.x = 1.0; a.y = 2.0; a.z = 3.0;\n"
        "  Vec3 b; b.x = 4.0; b.y = 5.0; b.z = 6.0;\n"
        "  return dot(a, b);\n"   // 4 + 10 + 18 = 32
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f();
            STRATA_CHECK(r > 31.9f && r < 32.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_nested_and_mixed_fields) {
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "struct Body { int id; Vec3 pos; };\n"
        "float entry() {\n"
        "  Body b;\n"
        "  b.id = 7;\n"
        "  b.pos = Vec3(1.0, 2.0, 3.0);\n"
        "  return b.pos.x + b.pos.y + b.pos.z + b.id;\n"   // 1+2+3+7 = 13
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f();
            STRATA_CHECK(r > 12.9f && r < 13.1f);
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_opaque_engine_handle) {
    // Engine-provided opaque type: Strata holds a handle and calls engine
    // functions on it, without knowing the engine's layout. Handles are
    // pointer-sized, so they cross the host<->JIT boundary cleanly.
    StrataJit* jit = compileJit(
        "handle Entity;\n"
        "extern Entity spawn();\n"
        "extern void despawn(Entity e);\n"
        "extern int id_of(Entity e);\n"
        "int run() {\n"
        "  Entity e = spawn();\n"
        "  int i = id_of(e);\n"
        "  despawn(e);\n"
        "  return i;\n"
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (!jit) return;

    auto spawn = +[]() -> void* { return reinterpret_cast<void*>(0xC0FFEE); };
    auto despawn = +[](void*) {};
    auto id_of = +[](void* e) -> int { return static_cast<int>(reinterpret_cast<intptr_t>(e)); };
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "spawn", (void*)spawn), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "despawn", (void*)despawn), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "id_of", (void*)id_of), 1);

    auto run = reinterpret_cast<int (*)()>(strataJitGetFunction(jit, "run"));
    STRATA_CHECK(run != nullptr);
    if (run) STRATA_CHECK_EQ(run(), 0xC0FFEE);
    strataJitDestroy(jit);
}

STRATA_TEST(jit_struct_zero_initialized_by_default) {
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "float entry() {\n"
        "  Vec3 v;\n"            // zero-init -> {0, 0, 0}
        "  v.x = 7.0;\n"         // -> {7, 0, 0}
        "  return v.x + v.y + v.z;\n"   // 7 + 0 + 0 = 7
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f();
            STRATA_CHECK(r > 6.9f && r < 7.1f);   // unwritten fields are zero
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_struct_inout_param_is_by_reference) {
    // Struct params are passed by reference: an inout write is visible to the
    // caller. (Copy explicitly with `Vec3 c = v;` for a local value.)
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "void bump(inout Vec3 v) { v.x = v.x + 10.0; v.y = v.y + 20.0; }\n"
        "float entry() {\n"
        "  Vec3 a; a.x = 1.0; a.y = 2.0; a.z = 3.0;\n"
        "  bump(a);\n"               // a becomes {11, 22, 3}
        "  return a.x + a.y + a.z;\n" // 11 + 22 + 3 = 36
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (jit) {
        auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
        STRATA_CHECK(f != nullptr);
        if (f) {
            float r = f();
            STRATA_CHECK(r > 35.9f && r < 36.1f);   // write-back through the reference
        }
        strataJitDestroy(jit);
    }
}

STRATA_TEST(jit_extern_struct_crosses_boundary_by_pointer) {
    // Structs cross the Strata->host boundary by pointer (in/out/inout), so the
    // host reads/writes through them. The host's Vec3 layout must match Strata's.
    struct HostVec3 { float x, y, z; };
    StrataJit* jit = compileJit(
        "struct Vec3 { float x; float y; float z; };\n"
        "extern float length_sq(in Vec3 v);\n"                          // host reads
        "extern void scale_into(in Vec3 src, float s, out Vec3 dst);\n" // host reads + writes
        "float entry() {\n"
        "  Vec3 v = Vec3(3.0, 4.0, 0.0);\n"
        "  Vec3 r;\n"
        "  scale_into(v, 2.0, r);\n"            // r = v * 2 = (6, 8, 0)
        "  return length_sq(v) + length_sq(r);\n" // 25 + 100 = 125
        "}\n");
    STRATA_CHECK(jit != nullptr);
    if (!jit) return;

    auto length_sq = +[](const HostVec3* v) -> float {
        return v->x * v->x + v->y * v->y + v->z * v->z;
    };
    auto scale_into = +[](const HostVec3* src, float s, HostVec3* dst) {
        dst->x = src->x * s;
        dst->y = src->y * s;
        dst->z = src->z * s;
    };
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "length_sq", (void*)length_sq), 1);
    STRATA_CHECK_EQ(strataJitAddSymbol(jit, "scale_into", (void*)scale_into), 1);

    auto f = reinterpret_cast<float (*)()>(strataJitGetFunction(jit, "entry"));
    STRATA_CHECK(f != nullptr);
    if (f) {
        float r = f();
        STRATA_CHECK(r > 124.9f && r < 125.1f);   // 125
    }
    strataJitDestroy(jit);
}

#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/LLVMAot.h"
#include <fstream>
STRATA_TEST(aot_emits_struct_object) {
    strata::DiagnosticEngine diag;
    auto mod = strata::test_util::parseModule(
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(in Vec3 a, in Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n", diag);
    STRATA_CHECK(!diag.hasErrors());
    std::string notes, err;
    strata::BuiltModule bm = strata::buildLLVMModule(*mod, notes);
    std::string path = "strata_struct_test.o";
    bool ok = strata::emitNativeFile(bm, path, false, err);
    if (!ok) std::printf("  AOT struct emission failed: %s\n", err.c_str());
    STRATA_CHECK(ok);
    std::ifstream in(path, std::ios::binary);
    STRATA_CHECK(in.good());
    in.seekg(0, std::ios::end);
    STRATA_CHECK(in.tellg() > 0);
}

#endif // STRATA_ENABLE_LLVM
