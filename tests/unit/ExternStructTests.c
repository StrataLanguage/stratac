#include "Util.h"
#include "Codegen/TypeRegistry.h"
#include "Test.h"
#include "strata/strata.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -- Parsing ----------------------------------------------------------- */

STRATA_TEST(parser_extern_struct_with_fieldoffset_and_fixed_arrays)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "extern struct Header {\n"
        "    fieldoffset(0) int magic;\n"
        "    fieldoffset(8) long size;\n"
        "    byte[16] name;\n"
        "    float[4] bbox;\n"
        "};\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 1);

    StructDecl* s = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(s->isExtern);
    STRATA_CHECK(!s->incomplete);
    STRATA_CHECK_EQ((long)s->fields.count, 4);

    FieldDecl* f0 = (FieldDecl*)VecGet(&s->fields, 0);
    STRATA_CHECK_EQ(f0->offset, 0);
    FieldDecl* f1 = (FieldDecl*)VecGet(&s->fields, 1);
    STRATA_CHECK_EQ(f1->offset, 8);
    FieldDecl* f2 = (FieldDecl*)VecGet(&s->fields, 2);
    STRATA_CHECK_EQ(f2->offset, -1);
    STRATA_CHECK(strcmp(f2->type.name, "byte[16]") == 0);
    STRATA_CHECK(f2->type.isArray);
    STRATA_CHECK_EQ(f2->type.length, 16);
    FieldDecl* f3 = (FieldDecl*)VecGet(&s->fields, 3);
    STRATA_CHECK(strcmp(f3->type.name, "float[4]") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_fixed_array_dimension_order_matches_c)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct M { int[2][6] grid; };\n", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StructDecl* s = (StructDecl*)VecGet(&mod->structs, 0);
    FieldDecl* f = (FieldDecl*)VecGet(&s->fields, 0);

    /* `int[2][6]` is 2 elements of int[6], mirroring C's int x[2][6]. */
    STRATA_CHECK(strcmp(f->type.name, "int[2][6]") == 0);
    STRATA_CHECK(f->type.isArray);
    STRATA_CHECK_EQ(f->type.length, 2);
    STRATA_CHECK(f->type.elem->isArray);
    STRATA_CHECK_EQ(f->type.elem->length, 6);
    STRATA_CHECK(strcmp(f->type.elem->name, "int[6]") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_fieldoffset_only_in_extern_struct)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseModule("struct S { fieldoffset(4) int x; };", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_extern_struct_requires_body)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    ParseModule("extern struct Ghost;", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* -- Sema restrictions -------------------------------------------------- */

static StrataResult CompileSource(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult result = strataCompileString(c, src, "extern_struct_tests", STRATA_EMIT_LLVM_IR, 0);
    strataCompilerDestroy(c);
    return result;
}

STRATA_TEST(sema_fixed_arrays_only_in_struct_fields)
{
    struct { const char* src; const char* msg; } cases[] = {
        {"int entry() { int[4] xs; return 0; }", "fixed-size array"},
        {"int f(int[4] xs) { return 0; }", "parameter"},
        {"int[4] f() { return 0; }", "return"},
        {"int[4] g = {1,2,3,4};", "global"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        StrataResult r = CompileSource(cases[i].src);
        STRATA_CHECK(!r.ok);
        if (r.diagnostics)
        {
            STRATA_CHECK(strstr(r.diagnostics, cases[i].msg) != NULL);
        }
        strataResultFree(&r);
    }
}

STRATA_TEST(sema_fixed_array_element_restrictions)
{
    struct { const char* src; const char* msg; } cases[] = {
        {"struct S { ^Foo[4] xs; };", "may not own its elements"},
        {"struct S { string[16] s; };", "may not own its elements"},
        {"struct S { int[0] xs; };", "at least 1"},
        /* `int[4][]` is a fixed array whose elements are dynamic arrays. */
        {"struct S { int[4][] xs; };", "may not contain a dynamic array"},
        /* `int[][4]` is a dynamic array whose elements are fixed arrays. */
        {"struct S { int[][4] xs; };", "only appear as (nested) fixed-size array struct fields"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        StrataResult r = CompileSource(cases[i].src);
        STRATA_CHECK(!r.ok);
        if (r.diagnostics)
        {
            STRATA_CHECK(strstr(r.diagnostics, cases[i].msg) != NULL);
        }
        strataResultFree(&r);
    }
}

STRATA_TEST(sema_fieldoffset_overlap_rejected)
{
    StrataResult r = CompileSource(
        "extern struct Bad {\n"
        "    fieldoffset(0) long a;\n"
        "    fieldoffset(4) int b;\n"
        "};");
    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "overlaps") != NULL);
    strataResultFree(&r);
}

STRATA_TEST(sema_extern_struct_redeclaration_conflict)
{
    StrataResult r = CompileSource(
        "extern struct E { int x; };\n"
        "extern struct E { long x; };\n");
    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "conflicting redeclaration") != NULL);
    strataResultFree(&r);

    /* An identical redeclaration is fine. */
    StrataResult ok = CompileSource(
        "extern struct E { int x; };\n"
        "extern struct E { int x; };\n"
        "int entry(E e) { return e.x; }\n");
    STRATA_CHECK(ok.ok);
    strataResultFree(&ok);
}

STRATA_TEST(sema_whole_fixed_field_assignment_rejected)
{
    StrataResult r = CompileSource(
        "extern struct S { byte[4] a; byte[4] b; };\n"
        "int entry(S s) { s.a = s.b; return 0; }");
    STRATA_CHECK(!r.ok);
    STRATA_CHECK(r.diagnostics && strstr(r.diagnostics, "whole fixed-size array") != NULL);
    strataResultFree(&r);
}

/* -- Layout computation -------------------------------------------------- */

STRATA_TEST(registry_layout_offsets_pads_and_size)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "extern struct L2 {\n"
        "    fieldoffset(0) byte a;\n"
        "    fieldoffset(8) long b;\n"
        "    int c;\n"
        "    byte[4] d;\n"
        "};\n"
        "struct Natural { byte a; int b; };\n",
        &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    const StructType* l2 = TypeRegistryFind(&reg, "L2");
    STRATA_CHECK(l2 != NULL);
    STRATA_CHECK(l2->hasLayout);
    STRATA_CHECK(l2->packedLayout);
    STRATA_CHECK(!l2->layoutError);
    STRATA_CHECK_EQ((long)l2->fields.count, 4);
    STRATA_CHECK_EQ(l2->fieldOffsets[0], 0);
    STRATA_CHECK_EQ(l2->fieldOffsets[1], 8);
    STRATA_CHECK_EQ(l2->fieldOffsets[2], 16); /* natural fill after offset 8+8 */
    STRATA_CHECK_EQ(l2->fieldOffsets[3], 20);
    STRATA_CHECK_EQ(l2->sizeBytes, 24); /* packed: exact cursor, no trailing pad */
    STRATA_CHECK_EQ(l2->alignBytes, 1);
    STRATA_CHECK_EQ((long)l2->padCount, 1); /* 7 bytes before field 1 */
    STRATA_CHECK_EQ((long)l2->pads[0].beforeField, 1);
    STRATA_CHECK_EQ(l2->pads[0].bytes, 7);
    STRATA_CHECK_EQ((long)l2->physicalCount, 5);
    STRATA_CHECK_EQ(l2->physicalIndex[0], 0);
    STRATA_CHECK_EQ(l2->physicalIndex[1], 2); /* after the pad member */
    STRATA_CHECK_EQ(l2->physicalIndex[2], 3);
    STRATA_CHECK_EQ(l2->physicalIndex[3], 4);

    const StructType* nat = TypeRegistryFind(&reg, "Natural");
    STRATA_CHECK(nat != NULL);
    STRATA_CHECK(nat->hasLayout);
    STRATA_CHECK(!nat->packedLayout);
    STRATA_CHECK_EQ((long)nat->padCount, 0);
    STRATA_CHECK_EQ(nat->sizeBytes, 8);
    STRATA_CHECK_EQ(nat->alignBytes, 4);
    STRATA_CHECK_EQ(nat->fieldOffsets[0], 0);
    STRATA_CHECK_EQ(nat->fieldOffsets[1], 4);

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* -- Emitted layout (IR + C text) ---------------------------------------- */

STRATA_TEST(emit_ir_layout_packed_with_pads)
{
    StrataResult r = CompileSource(
        "extern struct Layout { fieldoffset(0) byte a; fieldoffset(4) int b; };\n"
        "int entry(Layout l) { return l.a + l.b; }\n");
    STRATA_CHECK(r.ok);
    STRATA_CHECK(r.output != NULL);
    /* Packed body with an explicit 3-byte pad before field b. */
    STRATA_CHECK(strstr(r.output, "<{ i8, [3 x i8], i32 }>") != NULL);
    strataResultFree(&r);
}

STRATA_TEST(emit_c_layout_packed_with_pads_and_fixed_fields)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(
        c,
        "extern struct Layout { fieldoffset(0) byte a; fieldoffset(4) int b; byte[8] data; };\n"
        "int entry(Layout l) { return l.a + l.b + l.data[0]; }\n",
        "layout_c", STRATA_EMIT_C, 0);
    STRATA_CHECK(r.ok);
    STRATA_CHECK(r.output != NULL);
    STRATA_CHECK(strstr(r.output, "unsigned char strata__pad1[3];") != NULL);
    STRATA_CHECK(strstr(r.output, "__attribute__((packed))") != NULL);
    STRATA_CHECK(strstr(r.output, "unsigned char strata__field_data[8];") != NULL);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

/* -- JIT execution -------------------------------------------------------- */

STRATA_TEST(jit_extern_struct_fixed_array_ops)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(
        c,
        "extern struct Header {\n"
        "    fieldoffset(0) int magic;\n"
        "    fieldoffset(8) long size;\n"
        "    byte[16] name;\n"
        "    float[4] bbox;\n"
        "    int count;\n"
        "};\n"
        "int entry() {\n"
        "    Header h = Header { .magic = 1, .size = 2, .name = { 1, 2, 3 }, .bbox = { 10, 20 }, .count = 7 };\n"
        "    int total = h.magic + (int)h.size + h.count;\n"
        "    for (int i = 0; i < h.name.length; i++) { total += h.name[i]; }\n"
        "    for (int i = 0; i < h.bbox.length; i++) { total += (int)h.bbox[i]; }\n"
        "    return total;\n"
        "}\n",
        "fixed_jit", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        /* 1 + 2 + 7 + (1+2+3) + (10+20) = 46 */
        STRATA_CHECK_EQ(entry(), 46);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(jit_extern_struct_multidim_fixed_array)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(
        c,
        "extern struct Grid { int[2][3] cells; };\n"
        "int entry() {\n"
        "    Grid g = Grid { .cells = { 1, 2, 3, 4, 5, 6 } };\n"
        "    return g.cells[1][2] + g.cells.length + g.cells[0].length;\n"
        "}\n",
        "grid_jit", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        /* cells[1][2] = 6; .length = 2 (outer); cells[0].length = 3. */
        STRATA_CHECK_EQ(entry(), 11);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

/* -- Auto-drop of Strata-created instances ------------------------------- */

static long s_dropLive = 0;

static void* DropCountAlloc(unsigned long n)
{
    void* p = malloc((size_t)n);
    if (p)
    {
        s_dropLive++;
    }
    return p;
}

static void DropCountFree(void* p)
{
    if (p)
    {
        s_dropLive--;
        free(p);
    }
}

STRATA_TEST(jit_extern_struct_box_autodrop)
{
    StrataCompiler* c = strataCompilerCreate();
    strataJitSetAllocFreeFunctions(c, (void*)DropCountAlloc, (void*)DropCountFree);

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(
        c,
        "struct Counter { long n; };\n"
        "extern struct Payload {\n"
        "    fieldoffset(0) long id;\n"
        "    ^Counter c;\n"
        "    byte[4] tag;\n"
        "};\n"
        "long entry() {\n"
        "    ^Payload p = Payload { .id = 7, .c = Counter { .n = 5 }, .tag = { 1, 2 } };\n"
        "    return p.id + p.c.n + p.tag[0] + p.tag[1] + p.tag.length;\n"
        "}\n",
        "drop_jit", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    long (*entry)(void) = (long (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        /* 7 + 5 + 1 + 2 + 4 = 19 */
        STRATA_CHECK_EQ(entry(), 19);

        /* Both allocations (the ^Payload box and its ^Counter field) must be
           freed when the box drops at the end of entry(). */
        STRATA_CHECK_EQ(s_dropLive, 0);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
