#include "Util.h"
#include "Sema/ResolveOverloads.h"
#include "Test.h"

#if STRATA_TEST_HAS_LLVM
#include "Codegen/CodegenBackend.h"
#include "strata/strata.h"
#endif

#include <string.h>

/* ---- Parse + sema ---- */

STRATA_TEST(const_ref_scalar_parses_and_resolves)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int read(const ref int x) { return x; }\n"
        "int entry() { int v = 7; return read(v); }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count >= 1);

    FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(f->params.count == 1);

    ParamDecl* p = (ParamDecl*)VecGet(&f->params, 0);
    STRATA_CHECK(p->mod == ModRef);       /* by-reference */
    STRATA_CHECK(p->type.isConst);        /* read-only */

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(const_ref_and_ref_const_both_accepted)
{
    /* Either token order should parse to the same (by-ref, immutable) param. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "int a(const ref int x) { return x; }\n"
        "int b(ref const int x) { return x; }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fa = (FunctionDecl*)VecGet(&mod->functions, 0);
    FunctionDecl* fb = (FunctionDecl*)VecGet(&mod->functions, 1);
    ParamDecl* pa = (ParamDecl*)VecGet(&fa->params, 0);
    ParamDecl* pb = (ParamDecl*)VecGet(&fb->params, 0);

    STRATA_CHECK(pa->mod == ModRef && pa->type.isConst);
    STRATA_CHECK(pb->mod == ModRef && pb->type.isConst);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(const_ref_struct_is_allowed)
{
    /* Structs pass by value by default, so `const ref` is the explicit
       read-only VIEW spelling - it must not be rejected. */
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve(
        "struct V { float x; float y; };\n"
        "float first(const ref V v) { return v.x; }\n",
        &diag, &arena);

    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(assign_to_const_ref_scalar_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "int bad(const ref int x) { x = 5; return x; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(compound_assign_to_const_ref_scalar_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "int bad(const ref int x) { x += 5; return x; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(assign_to_const_ref_struct_member_is_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(
        "struct V { float x; float y; };\n"
        "int bad(const ref V v) { v.y = 9.0; return 0; }\n",
        &diag, &arena);

    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* ---- Const locals and const bypasses ---- */

/* True when `src` fails sema with a diagnostic containing `needle`. */
static bool ConstRejected(const char* src, const char* needle)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(src, &diag, &arena);

    SourceManager sm; SourceManagerInit(&sm);
    bool hit = DiagHasErrors(&diag) && strstr(DiagFormat(&diag, &sm, 1, &arena), needle) != NULL;

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return hit;
}

static bool ConstAccepted(const char* src)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    (void)ParseAndResolve(src, &diag, &arena);

    bool ok = !DiagHasErrors(&diag);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    return ok;
}

STRATA_TEST(const_local_assign_and_incdec_are_errors)
{
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x = 6; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x++; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int x = 5; x += 1; return x; }\n", "'x' is immutable"));
    STRATA_CHECK(ConstRejected("int entry() { const int[] a = {1, 2}; a[0] = 3; return a[0]; }\n",
                               "'a' is immutable"));
}

STRATA_TEST(const_local_is_readable_and_scoped)
{
    /* A const local can be read and copied; a later sibling-scope name is a fresh mutable binding. */
    STRATA_CHECK(ConstAccepted(
        "int entry() {\n"
        "  const int x = 5; int y = x; y++;\n"
        "  if (y > 0) { const int z = 1; y = y + z; }\n"
        "  int z = 2; z = 3;\n"
        "  return y + z;\n"
        "}\n"));
}

STRATA_TEST(const_ref_arg_to_mutable_ref_param_is_error)
{
    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void m(ref P p) { p.v = 1; }\n"
        "void g(const ref P p) { m(p); }\n",
        "'p' is immutable and cannot be passed to non-const 'ref' parameter"));

    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void setv(ref int x) { x = 1; }\n"
        "void g(const ref P p) { setv(p.v); }\n",
        "'p' is immutable"));

    STRATA_CHECK(ConstRejected(
        "void setv(ref int x) { x = 1; }\n"
        "int entry() { const int x = 5; setv(x); return x; }\n",
        "'x' is immutable"));
}

STRATA_TEST(const_ref_arg_to_const_ref_param_is_allowed)
{
    STRATA_CHECK(ConstAccepted(
        "struct P { int v; };\n"
        "int m(const ref P p) { return p.v; }\n"
        "int g(const ref P p) { return m(p); }\n"
        "int h(const int[] a) { return a.length; }\n"));
}

STRATA_TEST(const_array_push_pop_resize_are_errors)
{
    STRATA_CHECK(ConstRejected("void g(const int[] a) { array_push(a, 1); }\n", "it is immutable"));
    STRATA_CHECK(ConstRejected("int g(const int[] a) { return array_pop(a); }\n", "it is immutable"));
    STRATA_CHECK(ConstRejected("void g(const int[] a) { array_resize(a, 4); }\n", "it is immutable"));
}

STRATA_TEST(const_box_drop_is_error)
{
    STRATA_CHECK(ConstRejected(
        "struct P { int v; };\n"
        "void g(const ^P p) { drop(p); }\n",
        "cannot drop 'p'; it is immutable"));
}

/* ---- Moving out of a const ref ---- */

/* A const ref only borrows, and nothing can be put back through it: moving an owning part out (a
   struct field, an element, a set optional) is an error, like moving out of a global. */
static const char* HOLDER =
    "struct Box { int v; };\n"
    "struct H { ulong id; rc<Box> view; };\n"
    "struct Holder { H tex; H? opt; H[] mips; string name; rc<Box> shared; };\n";

static bool HolderRejected(const char* body, const char* needle)
{
    Arena arena; arena_init(&arena, 0);
    bool hit = ConstRejected(arena_format(&arena, "%s%s", HOLDER, body), needle);
    arena_free(&arena);

    return hit;
}

static bool HolderAccepted(const char* body)
{
    Arena arena; arena_init(&arena, 0);
    bool ok = ConstAccepted(arena_format(&arena, "%s%s", HOLDER, body));
    arena_free(&arena);

    return ok;
}

STRATA_TEST(move_out_of_const_ref_param_is_error)
{
    STRATA_CHECK(HolderRejected("H f(const ref Holder self) { return self.tex; }\n",
                                "'self.tex' cannot be moved as it is not owned because 'self' is a const ref"));
    STRATA_CHECK(HolderRejected("H f(const ref Holder self, uint i) { return self.mips[i]; }\n",
                                "'self.mips[i]' cannot be moved"));
    STRATA_CHECK(HolderRejected(
        "H f(const ref Holder self) { if (self.opt?) { return self.opt; } return copy(self.tex); }\n",
        "'self.opt' cannot be moved"));
    STRATA_CHECK(HolderRejected("void f(const ref Holder self) { H? o = self.opt; }\n", "'self.opt' cannot be moved"));
    STRATA_CHECK(HolderRejected("void f(const ref Holder self) { string s = self.name; }\n",
                                "'self.name' cannot be moved"));
    STRATA_CHECK(HolderRejected(
        "string keep(string s) { return s; }\n"
        "void f(const ref Holder self) { string k = keep(self.name); }\n",
        "'self.name' cannot be moved"));
}

STRATA_TEST(move_out_of_const_ref_local_is_error)
{
    STRATA_CHECK(HolderRejected("H f(ref Holder x) { const ref Holder h = x; return h.tex; }\n",
                                "'h.tex' cannot be moved as it is not owned because 'h' is a const ref"));
    STRATA_CHECK(HolderRejected("void f(ref Holder x) { const ref Holder h = x; H? o = h.opt; }\n",
                                "'h.opt' cannot be moved as it is not owned because 'h' is a const ref"));
}

STRATA_TEST(const_ref_read_copy_and_borrow_are_fine)
{
    STRATA_CHECK(HolderAccepted(
        "int len(string s) { return (int)s.length; }\n"
        "H tex(const ref Holder self) { return copy(self.tex); }\n"
        "H opt(const ref Holder self) {\n"
        "  if (self.opt?) { const ref H o = self.opt; return copy(o); }\n"
        "  return copy(self.tex);\n"
        "}\n"
        "H mip(const ref Holder self, uint i) { return copy(self.mips[i]); }\n"
        "rc<Box> shared(const ref Holder self) { return self.shared; }\n" /* copying an rc retains */
        "rc<Box> view(const ref Holder self) { return self.tex.view; }\n"
        "void take(H h) { drop(h); }\n"
        "void give(const ref Holder self) { take(self.tex); }\n" /* a by-value struct argument is a copy */
        "int f(const ref Holder self) {\n"
        "  const ref H t = self.tex;\n"
        "  return len(self.name) + (int)t.id + self.tex.view.v + (int)self.mips.length;\n"
        "}\n"));
}

STRATA_TEST(move_out_of_ref_param_field_still_allowed)
{
    /* Only const refs are covered: take-and-refill through a mutable `ref` keeps working. */
    STRATA_CHECK(HolderAccepted("H swap(ref Holder self, H next) { ^H old = self.tex; self.tex = next; return old; }\n"));
}

#if STRATA_TEST_HAS_LLVM


/* ---- JIT runtime behavior ---- */

STRATA_TEST(const_ref_scalar_is_readable_view)
{
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "int read(const ref int x) { return x + 1; }\n"
        "int entry() {\n"
        "  int v = 10;\n"
        "  int r = read(v);\n"            /* 11; v unchanged (view, not move) */
        "  return r + v;\n"               /* 11 + 10 = 21 */
        "}\n",
        "cref", &err);
    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        return;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 21);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(const_ref_struct_dot_product_runs)
{
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "struct Vec3 { float x; float y; float z; };\n"
        "float dot(const ref Vec3 a, const ref Vec3 b) {\n"
        "  return a.x * b.x + a.y * b.y + a.z * b.z;\n"
        "}\n"
        "int entry() {\n"
        "  Vec3 v = Vec3 { .x = 1.0, .y = 2.0, .z = 3.0 };\n"
        "  return (int)dot(v, v);\n"      /* 1 + 4 + 9 = 14 */
        "}\n",
        "cref", &err);
    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        return;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 14);
    }

    strataJitDestroy(jit);
}

STRATA_TEST(const_ref_getter_copies_leave_the_holder_intact)
{
    /* Getters on a const ref copy, so calling them twice sees the same values and the holder keeps its own. */
    const char* err = NULL;
    StrataCompiler* c = strataCompilerCreate();
    StrataJit* jit = strataJitCompileString(
        c,
        "struct Box { int v; };\n"
        "struct H { ulong id; rc<Box> view; };\n"
        "struct Holder { H tex; H? opt; H[] mips; };\n"
        "H tex(const ref Holder self) { return copy(self.tex); }\n"
        "H opt(const ref Holder self) {\n"
        "  if (self.opt?) { const ref H o = self.opt; return copy(o); }\n"
        "  return copy(self.tex);\n"
        "}\n"
        "H mip(const ref Holder self, uint i) { return copy(self.mips[i]); }\n"
        "int entry() {\n"
        "  rc<Box> b = Box { .v = 7 };\n"
        "  ^Holder h = Holder { .tex = H { .id = 1ul, .view = b } };\n"
        "  h.opt = H { .id = 2ul, .view = b };\n"
        "  array_push(h.mips, H { .id = 3ul, .view = b });\n"
        "  ^H a = tex(h); ^H a2 = tex(h);\n"
        "  ^H o = opt(h); ^H o2 = opt(h);\n"
        "  ^H m = mip(h, 0u); ^H m2 = mip(h, 0u);\n"
        "  if (!h.opt?) { return 100; }\n"
        "  return a.view.v + a2.view.v + o.view.v + o2.view.v + m.view.v + m2.view.v\n"
        "         + h.tex.view.v + h.mips[0].view.v + (int)(o2.id + m2.id);\n" /* 8 * 7 + 5 */
        "}\n",
        "cref_getters", &err);
    strataCompilerDestroy(c);

    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        strataFree((char*)err);
        return;
    }

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 61);
    }

    strataJitDestroy(jit);
}


#endif
