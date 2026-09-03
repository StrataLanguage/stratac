#include "Util.h"
#if STRATA_TEST_HAS_LLVM
#include "strata/strata.h"
#endif
#include "Codegen/CodegenBackend.h"
#include "Sema/ResolveOverloads.h"
#include "Test.h"

#include <stdlib.h>
#include <string.h>

static bool Contains(const char* h, const char* n)
{
    return strstr(h, n) != NULL;
}

static const FunctionDecl* FindFunction(const Module* mod, const char* name)
{
    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* f = (const FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(f->name, name) == 0)
        {
            return f;
        }
    }

    return NULL;
}

static const char* ErrText(DiagnosticEngine* diag, Arena* arena)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < diag->m_count; i++)
    {
        SbPrintf(&sb, "%s; ", diag->m_diagnostics[i].message);
    }

    return SbFinish(&sb, arena);
}

STRATA_TEST(impl_parses_methods_and_properties)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("handle Camera;\n"
                              "impl Camera {\n"
                              "    extern float GetFOV(Camera self);\n"
                              "    extern void SetFOV(Camera self, float fov);\n"
                              "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                              "    property float3 Direction { get = Camera_GetDirection; }\n"
                              "}\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod != NULL);
    STRATA_CHECK_EQ(mod->impls.count, 1);

    ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, 0);
    STRATA_CHECK(strcmp(impl->handleName, "Camera") == 0);
    STRATA_CHECK_EQ(impl->methods.count, 2);
    STRATA_CHECK_EQ(impl->properties.count, 2);

    FunctionDecl* m0 = (FunctionDecl*)VecGet(&impl->methods, 0);
    STRATA_CHECK(m0->isExtern);
    STRATA_CHECK(m0->fromImpl);
    STRATA_CHECK(strcmp(m0->methodName, "GetFOV") == 0);
    STRATA_CHECK(strcmp(m0->name, "Camera_GetFOV") == 0);
    STRATA_CHECK_EQ(m0->params.count, 1);

    PropertyDecl* p0 = (PropertyDecl*)VecGet(&impl->properties, 0);
    STRATA_CHECK(strcmp(p0->name, "FOV") == 0);
    STRATA_CHECK(strcmp(p0->returnType.name, "float") == 0);
    STRATA_CHECK(strcmp(p0->getterSymbol, "Camera_GetFOV") == 0);
    STRATA_CHECK(strcmp(p0->setterSymbol, "Camera_SetFOV") == 0);

    PropertyDecl* p1 = (PropertyDecl*)VecGet(&impl->properties, 1);
    STRATA_CHECK(strcmp(p1->name, "Direction") == 0);
    STRATA_CHECK(strcmp(p1->returnType.name, "float3") == 0);
    STRATA_CHECK(p1->getterSymbol != NULL);
    STRATA_CHECK(p1->setterSymbol == NULL);

    /* Methods are hoisted into the module function list. */
    STRATA_CHECK(FindFunction(mod, "Camera_GetFOV") != NULL);
    STRATA_CHECK(FindFunction(mod, "Camera_SetFOV") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_rejects_body_and_duplicates)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseModule("handle Camera;\n"
                "impl Camera {\n"
                "    float GetFOV(Camera self) { return 0.0; }\n"
                "}\n",
                &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseModule("handle Camera;\n"
                "impl Camera {\n"
                "    extern float GetFOV(Camera self);\n"
                "    extern float GetFOV(Camera self);\n"
                "}\n",
                &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_requires_handle_target)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Vec { float x; }\n"
                    "impl Vec {\n"
                    "    extern float Get(Vec self);\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "not a declared handle"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_method_call_rewrites_to_extern)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("handle Camera;\n"
                                  "impl Camera {\n"
                                  "    extern float GetFOV(Camera self);\n"
                                  "}\n"
                                  "float entry(Camera c) { return c.GetFOV(); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* entry = (FunctionDecl*)FindFunction(mod, "entry");
    STRATA_CHECK(entry != NULL);

    Block* body = (Block*)entry->body;
    ReturnStmt* ret = (ReturnStmt*)VecGet(&body->statements, 0);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "Camera_GetFOV") == 0);
    STRATA_CHECK(call->resolvedDecl != NULL);
    STRATA_CHECK_EQ(call->args.count, 1);
    STRATA_CHECK(((Node*)VecGet(&call->args, 0))->kind == NodeIdent);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_method_with_args_prepends_self)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("handle Camera;\n"
                                  "impl Camera {\n"
                                  "    extern void Rotate(Camera self, float3 axis, float radians);\n"
                                  "}\n"
                                  "void entry(Camera c) { c.Rotate(float3(0.0, 1.0, 0.0), 1.0); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* entry = (FunctionDecl*)FindFunction(mod, "entry");
    Block* body = (Block*)entry->body;
    ExprStmt* stmt = (ExprStmt*)VecGet(&body->statements, 0);
    STRATA_CHECK(stmt->expr->kind == NodeCall);

    CallExpr* call = (CallExpr*)stmt->expr;
    STRATA_CHECK(strcmp(call->callee, "Camera_Rotate") == 0);
    STRATA_CHECK_EQ(call->args.count, 3);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_static_factory_call_has_no_self)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("handle Camera;\n"
                                  "impl Camera {\n"
                                  "    extern Camera New();\n"
                                  "}\n"
                                  "Camera make() { return Camera.New(); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* make = (FunctionDecl*)FindFunction(mod, "make");
    Block* body = (Block*)make->body;
    ReturnStmt* ret = (ReturnStmt*)VecGet(&body->statements, 0);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "Camera_New") == 0);
    STRATA_CHECK_EQ(call->args.count, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_inherited_method_visible_on_derived)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("handle Entity;\n"
                                  "handle Player extends Entity;\n"
                                  "impl Entity {\n"
                                  "    extern void Hit(Entity self, int dmg);\n"
                                  "}\n"
                                  "void entry(Player p) { p.Hit(3); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* entry = (FunctionDecl*)FindFunction(mod, "entry");
    Block* body = (Block*)entry->body;
    ExprStmt* stmt = (ExprStmt*)VecGet(&body->statements, 0);

    CallExpr* call = (CallExpr*)stmt->expr;
    STRATA_CHECK(strcmp(call->callee, "Entity_Hit") == 0);
    STRATA_CHECK_EQ(call->args.count, 2);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_property_read_and_write)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("handle Camera;\n"
                                  "extern float Camera_GetFOV(Camera self);\n"
                                  "extern void Camera_SetFOV(Camera self, float v);\n"
                                  "impl Camera {\n"
                                  "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                                  "}\n"
                                  "float entry(Camera c) { return c.FOV; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    /* The user-declared flat externs must not be duplicated by synthesis. */
    int getFovCount = 0;
    int setFovCount = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(f->name, "Camera_GetFOV") == 0)
        {
            getFovCount++;
            STRATA_CHECK(!f->fromImpl);
        }

        if (strcmp(f->name, "Camera_SetFOV") == 0)
        {
            setFovCount++;
        }
    }

    STRATA_CHECK_EQ(getFovCount, 1);
    STRATA_CHECK_EQ(setFovCount, 1);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Write path: type-checked against the property type. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    Module* mod2 = ParseAndResolve("handle Camera;\n"
                                   "impl Camera {\n"
                                   "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                                   "}\n"
                                   "void entry(Camera c) { c.FOV = 90.0; }\n",
                                   &diag2, &arena2);
    STRATA_CHECK(!DiagHasErrors(&diag2));

    /* Synthesized setter extern: void Set(Camera self, float value). */
    const FunctionDecl* setter = FindFunction(mod2, "Camera_SetFOV");
    STRATA_CHECK(setter != NULL);
    STRATA_CHECK(setter->isExtern);
    STRATA_CHECK(strcmp(setter->returnType.name, "void") == 0);
    STRATA_CHECK_EQ(setter->params.count, 2);

    const ParamDecl* selfParam = (const ParamDecl*)VecGet(&setter->params, 0);
    STRATA_CHECK(strcmp(selfParam->type.name, "Camera") == 0);

    const ParamDecl* valueParam = (const ParamDecl*)VecGet(&setter->params, 1);
    STRATA_CHECK(strcmp(valueParam->type.name, "float") == 0);

    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_property_type_mismatch_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV = \"nope\"; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_readonly_write_and_writeonly_read_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float Camera_GetFOV(Camera self);\n"
                    "    property float FOV { get = Camera_GetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV = 1.0; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "read-only"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern void Camera_SetFOV(Camera self, float v);\n"
                    "    property float FOV { set = Camera_SetFOV; }\n"
                    "}\n"
                    "float entry(Camera c) { return c.FOV; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "write-only"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_compound_and_incdec_on_property_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV += 1.0; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV++; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_unknown_members_diagnosed)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float GetFOV(Camera self);\n"
                    "}\n"
                    "float entry(Camera c) { return c.Position; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "no member"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float GetFOV(Camera self);\n"
                    "}\n"
                    "float entry(Camera c) { return c.Missing(); }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "no method"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);

    /* Bare method access (no call) is diagnosed too. */
    Arena arena3;
    arena_init(&arena3, 0);
    DiagnosticEngine diag3;
    DiagnosticEngineInit(&diag3);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float GetFOV(Camera self);\n"
                    "}\n"
                    "void entry(Camera c) { c.GetFOV; }\n",
                    &diag3, &arena3);
    STRATA_CHECK(DiagHasErrors(&diag3));
    STRATA_CHECK(Contains(ErrText(&diag3, &arena3), "must be called"));
    DiagnosticEngineFree(&diag3);
    arena_free(&arena3);
}

STRATA_TEST(impl_property_call_syntax_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
                    "}\n"
                    "float entry(Camera c) { return c.FOV(); }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "is a property"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_optional_receiver_needs_blessing)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float GetFOV(Camera self);\n"
                    "}\n"
                    "float entry(Camera? c) { return c.GetFOV(); }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "bless"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Blessed receiver (via `if (c?)`) is fine. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern float GetFOV(Camera self);\n"
                    "}\n"
                    "float entry(Camera? c)\n"
                    "{\n"
                    "    if (c?)\n"
                    "    {\n"
                    "        return c.GetFOV();\n"
                    "    }\n"
                    "    return 0.0;\n"
                    "}\n",
                    &diag2, &arena2);
    STRATA_CHECK(!DiagHasErrors(&diag2));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_astdump_shows_impl_block)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("handle Camera;\n"
                              "impl Camera {\n"
                              "    extern float GetFOV(Camera self);\n"
                              "    property float FOV { get = Camera_GetFOV; }\n"
                              "}\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    char* dump = DumpAst(mod, &arena);
    STRATA_CHECK(Contains(dump, "impl Camera {"));
    STRATA_CHECK(Contains(dump, "extern float GetFOV("));
    STRATA_CHECK(Contains(dump, "property float FOV { get = Camera_GetFOV; }"));
    /* Hoisted methods are not printed twice at module scope. */
    STRATA_CHECK(!Contains(dump, "Camera_GetFOV("));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

#if STRATA_TEST_HAS_LLVM
static float Camera_GetFOVThunk(void* self)
{
    return *(float*)self;
}

static void Camera_SetFOVThunk(void* self, float v)
{
    *(float*)self = v;
}

static void* Camera_NewThunk(void)
{
    float* cell = (float*)malloc(sizeof(float));
    *cell = 0.0f;
    return cell;
}

static void* Camera_GetSelfThunk(void* self)
{
    return self;
}

STRATA_TEST(impl_jit_end_to_end)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "handle Camera;\n"
        "impl Camera {\n"
        "    extern float GetFOV(Camera self);\n"
        "    extern void SetFOV(Camera self, float v);\n"
        "    extern Camera New();\n"
        "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
        "}\n"
        "float entry(Camera cam)\n"
        "{\n"
        "    cam.FOV = 42.0;\n"
        "    return cam.FOV + cam.GetFOV();\n"
        "}\n"
        "Camera make() { return Camera.New(); }\n",
        "impl_test", &err);

    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_GetFOV", (void*)&Camera_GetFOVThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_SetFOV", (void*)&Camera_SetFOVThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_New", (void*)&Camera_NewThunk));

    float (*entry)(void*) = (float (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        float cell = 1.0f;
        STRATA_CHECK_EQ((long)(entry(&cell) * 100.0f), 8400); /* 42 + 42 */
    }

    void* (*make)(void) = (void* (*)(void))strataJitGetFunction(jit, "make");
    STRATA_CHECK(make != NULL);

    if (make)
    {
        void* cam = make();
        STRATA_CHECK(cam != NULL);
        free(cam);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_jit_property_on_array_elem)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "handle Camera;\n"
        "impl Camera {\n"
        "    extern float GetFOV(Camera self);\n"
        "    extern void SetFOV(Camera self, float v);\n"
        "    extern Camera New();\n"
        "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
        "}\n"
        "float entry()\n"
        "{\n"
        "    Camera a = Camera.New();\n"
        "    Camera b = Camera.New();\n"
        "    Camera[] cams = { a, b };\n"
        "    cams[0].FOV = 7.0;\n"
        "    cams[1].FOV = 9.0;\n"
        "    return cams[0].FOV + cams[1].FOV;\n"
        "}\n",
        "impl_arr", &err);

    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_GetFOV", (void*)&Camera_GetFOVThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_SetFOV", (void*)&Camera_SetFOVThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_New", (void*)&Camera_NewThunk));

    /* The array is built inside Strata (a `T[]` param owns its buffer, so
       passing a host stack buffer here would be freed by the callee's drop
       glue and corrupt the heap). */
    float (*entry)(void) = (float (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        float sum = entry();
        STRATA_CHECK_EQ((long)(sum * 100.0f), 1600); /* 7 + 9 */
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_jit_property_through_struct_field)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "handle Camera;\n"
        "struct Holder { Camera cam; };\n"
        "impl Camera {\n"
        "    extern float GetFOV(Camera self);\n"
        "    extern void SetFOV(Camera self, float v);\n"
        "    property float FOV { get = Camera_GetFOV; set = Camera_SetFOV; }\n"
        "}\n"
        "float entry(Holder h)\n"
        "{\n"
        "    h.cam.FOV = 5.5;\n"
        "    return h.cam.FOV;\n"
        "}\n",
        "impl_field", &err);

    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_GetFOV", (void*)&Camera_GetFOVThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "Camera_SetFOV", (void*)&Camera_SetFOVThunk));

    typedef struct
    {
        void* cam;
    } Holder;

    float (*entry)(Holder*) = (float (*)(Holder*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        float cell = 1.0f;
        Holder h = {&cell};

        float v = entry(&h);
        STRATA_CHECK_EQ((long)(v * 100.0f), 550);
        STRATA_CHECK_EQ((long)(cell * 100.0f), 550);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_jit_recursive_property_chain)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "handle Camera;\n"
        "impl Camera {\n"
        "    extern Camera GetSelf(Camera self);\n"
        "    extern float GetFOV(Camera self);\n"
        "    property Camera Self { get = GetSelf; }\n"
        "    property float FOV { get = GetFOV; }\n"
        "}\n"
        "float entry(Camera c) { return c.Self.Self.Self.FOV; }\n",
        "impl_rec", &err);

    if (!jit)
    {
        printf("  JIT compile failed: %s\n", err ? err : "(no message)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        STRATA_CHECK(false);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "GetSelf", (void*)&Camera_GetSelfThunk));
    STRATA_CHECK(strataJitAddSymbol(jit, "GetFOV", (void*)&Camera_GetFOVThunk));

    float (*entry)(void*) = (float (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);

    if (entry)
    {
        float cell = 3.25f;
        STRATA_CHECK_EQ((long)(entry(&cell) * 100.0f), 325);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
#endif
