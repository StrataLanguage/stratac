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

STRATA_TEST(impl_allows_inline_body_and_rejects_duplicates)
{
    /* An impl method may be DEFINED inline (non-extern, with a body) - it
       hoists to the qualified symbol like any other method. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("handle Camera;\n"
                              "impl Camera {\n"
                              "    float GetFOV(Camera self) { return 0.0; }\n"
                              "}\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod != NULL);
    if (mod)
    {
        STRATA_CHECK_EQ(mod->impls.count, 1);
        ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, 0);
        STRATA_CHECK_EQ(impl->methods.count, 1);
        FunctionDecl* m0 = (FunctionDecl*)VecGet(&impl->methods, 0);
        STRATA_CHECK(!m0->isExtern);
        STRATA_CHECK(m0->fromImpl);
        STRATA_CHECK(m0->body != NULL);
        STRATA_CHECK(strcmp(m0->methodName, "GetFOV") == 0);
        STRATA_CHECK(strcmp(m0->name, "Camera_GetFOV") == 0);
        STRATA_CHECK(FindFunction(mod, "Camera_GetFOV") != NULL);
    }
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Duplicate methods (extern or inline) are still rejected. */
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

    Arena arena3;
    arena_init(&arena3, 0);
    DiagnosticEngine diag3;
    DiagnosticEngineInit(&diag3);
    ParseModule("handle Camera;\n"
                "impl Camera {\n"
                "    float GetFOV(Camera self) { return 0.0; }\n"
                "    float GetFOV(Camera self) { return 1.0; }\n"
                "}\n",
                &diag3, &arena3);
    STRATA_CHECK(DiagHasErrors(&diag3));
    DiagnosticEngineFree(&diag3);
    arena_free(&arena3);
}

STRATA_TEST(impl_requires_handle_or_struct_target)
{
    /* impl targets any registered type (handles, defined structs,
       forward-declared structs, and type aliases). Only undeclared types are
       rejected. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("impl Ghost {\n"
                    "    extern float Get(Ghost self);\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "not a declared struct or handle"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("struct Meter = int;\n"
                    "impl Meter {\n"
                    "    extern int Get(Meter self);\n"
                    "}\n",
                    &diag2, &arena2);
    STRATA_CHECK(!DiagHasErrors(&diag2));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
}

STRATA_TEST(impl_on_forward_declared_struct_parses)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct TheType;\n"
                    "impl TheType {\n"
                    "    extern int Get(TheType self);\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_on_type_alias_scalar_method_rewrites)
{
    /* `impl` on a strong alias of a scalar: `expr.M(args)` hoists to the
       qualified extern `Meter_Get` with self prepended, exactly like a handle
       or struct impl. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Meter = int;\n"
                                  "impl Meter {\n"
                                  "    extern int Get(Meter self);\n"
                                  "}\n"
                                  "int entry(Meter m) { return m.Get(); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* entry = (FunctionDecl*)FindFunction(mod, "entry");
    STRATA_CHECK(entry != NULL);

    Block* body = (Block*)entry->body;
    ReturnStmt* ret = (ReturnStmt*)VecGet(&body->statements, 0);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "Meter_Get") == 0);
    STRATA_CHECK(call->resolvedDecl != NULL);
    STRATA_CHECK_EQ(call->args.count, 1);
    STRATA_CHECK(((Node*)VecGet(&call->args, 0))->kind == NodeIdent);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_on_type_alias_struct_method_rewrites)
{
    /* `impl` on a strong alias of a struct: the alias is a distinct impl
       target from its underlying (no method leaks across). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Foo { int x; };\n"
                                  "struct X = Foo;\n"
                                  "impl X {\n"
                                  "    extern int GetX(X self);\n"
                                  "}\n"
                                  "int entry(X x) { return x.GetX(); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* entry = (FunctionDecl*)FindFunction(mod, "entry");
    STRATA_CHECK(entry != NULL);

    Block* body = (Block*)entry->body;
    ReturnStmt* ret = (ReturnStmt*)VecGet(&body->statements, 0);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "X_GetX") == 0);
    STRATA_CHECK(call->resolvedDecl != NULL);
    STRATA_CHECK_EQ(call->args.count, 1);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_on_type_alias_static_factory_has_no_self)
{
    /* Parameterless impl methods on an alias resolve as static calls. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct Meter = int;\n"
                                  "impl Meter {\n"
                                  "    extern Meter New();\n"
                                  "}\n"
                                  "Meter make() { return Meter.New(); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* make = (FunctionDecl*)FindFunction(mod, "make");
    STRATA_CHECK(make != NULL);

    Block* body = (Block*)make->body;
    ReturnStmt* ret = (ReturnStmt*)VecGet(&body->statements, 0);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "Meter_New") == 0);
    STRATA_CHECK_EQ(call->args.count, 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(impl_on_type_alias_distinct_from_underlying)
{
    /* The alias's impl is attached to the alias NAME only: the underlying
       type does not see the method, and calls on the underlying are rejected
       (no implicit conversion between an alias and its underlying). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("struct Meter = int;\n"
                    "impl Meter {\n"
                    "    extern int Get(Meter self);\n"
                    "}\n"
                    "int entry() { int i = 5; return i.Get(); }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "no method 'Get'"));

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

STRATA_TEST(impl_property_overloaded_accessor_rejected)
{
    /* A get/set symbol that names multiple functions (overloads) is ambiguous:
       the accessor must be a unique function, regardless of which overload's
       shape happens to appear first in the module. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "int Camera_GetFOV(Camera self) { return 1; }\n"
                    "int Camera_GetFOV(Camera self, int mode) { return 2; }\n"
                    "impl Camera {\n"
                    "    property int FOV { get = Camera_GetFOV; }\n"
                    "}\n"
                    "int entry(Camera c) { return c.FOV; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "is ambiguous"));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "Camera_GetFOV"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Same rule applies to the setter symbol. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "void Camera_SetFOV(Camera self, int v) {}\n"
                    "void Camera_SetFOV(Camera self, int v, int w) {}\n"
                    "impl Camera {\n"
                    "    property int FOV { set = Camera_SetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV = 1; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "is ambiguous"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);

    /* A unique accessor symbol still compiles (no ambiguity). */
    Arena arena3;
    arena_init(&arena3, 0);
    DiagnosticEngine diag3;
    DiagnosticEngineInit(&diag3);
    ParseAndResolve("handle Camera;\n"
                    "int Camera_GetFOV(Camera self) { return 1; }\n"
                    "int Camera_GetFOV(Camera self, int mode) { return 2; }\n"
                    "int Camera_GetZoom(Camera self) { return 3; }\n"
                    "impl Camera {\n"
                    "    property int FOV { get = Camera_GetZoom; }\n"
                    "}\n"
                    "int entry(Camera c) { return c.FOV; }\n",
                    &diag3, &arena3);
    STRATA_CHECK(!DiagHasErrors(&diag3));
    DiagnosticEngineFree(&diag3);
    arena_free(&arena3);
}

STRATA_TEST(impl_property_accessor_shape_mismatch_rejected)
{
    /* Setter value param must be the property's type, not just any (self, T). */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern void SetFOV(Camera self, float v);\n"
                    "    property int FOV { set = Camera_SetFOV; }\n"
                    "}\n"
                    "void entry(Camera c) { c.FOV = 1; }\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "must take 'int' as its value parameter"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);

    /* Getter with a `return` out-param whose type is not the property type. */
    Arena arena2;
    arena_init(&arena2, 0);
    DiagnosticEngine diag2;
    DiagnosticEngineInit(&diag2);
    ParseAndResolve("handle Camera;\n"
                    "impl Camera {\n"
                    "    extern void GetFOV(Camera self, return float v);\n"
                    "    property int FOV { get = Camera_GetFOV; }\n"
                    "}\n"
                    "int entry(Camera c) { return c.FOV; }\n",
                    &diag2, &arena2);
    STRATA_CHECK(DiagHasErrors(&diag2));
    STRATA_CHECK(Contains(ErrText(&diag2, &arena2), "must return 'int'; found 'float'"));
    DiagnosticEngineFree(&diag2);
    arena_free(&arena2);
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
/* Host side of impl_on_opaque_struct_jit: TheType is never defined in Strata;
   the host owns the layout and hands boxes (opaque pointers) across. */
static void OpaqueGetTypeImpl(void** out)
{
    int* p = (int*)malloc(sizeof(int));
    if (p)
    {
        *p = 42;
    }
    *out = p;
}

static int  OpaqueGetValueImpl(void* self)        { return *((int*)self); }
static void OpaqueSetValueImpl(void* self, int v) { *((int*)self) = v; }
static int  OpaqueBumpImpl(void* self)
{
    int old = *((int*)self);
    *((int*)self) = old + 1;
    return old;
}

STRATA_TEST(impl_on_opaque_struct_jit)
{
    /* A forward-declared struct with no body, driven opaquely through a
       `^TheType` box (return param) and impl-declared extern methods, via the
       public embed API. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct TheType;\n"
        "extern void GetType(return ^TheType t);\n"
        "impl TheType {\n"
        "    extern int GetValue(TheType self);\n"
        "    extern void SetValue(TheType self, int v);\n"
        "    extern int Bump(TheType self);\n"
        "    property int Value { get = TheType_GetValue; set = TheType_SetValue; }\n"
        "}\n"
        "int entry()\n"
        "{\n"
        "  ^TheType t = GetType();\n"      /* box { 42 } */
        "  int v = t.Value;\n"             /* getter: 42 */
        "  t.Value = v + 1;\n"             /* setter: host stores 43 */
        "  int w = t.Bump();\n"            /* reads 43, stores 44, returns 43 */
        "  return w + t.Value;\n"          /* 43 + 44 = 87 */
        "}\n",
        "opaque_impl", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "GetType", (void*)&OpaqueGetTypeImpl));
    STRATA_CHECK(strataJitAddSymbol(jit, "TheType_GetValue", (void*)&OpaqueGetValueImpl));
    STRATA_CHECK(strataJitAddSymbol(jit, "TheType_SetValue", (void*)&OpaqueSetValueImpl));
    STRATA_CHECK(strataJitAddSymbol(jit, "TheType_Bump", (void*)&OpaqueBumpImpl));

    int (*entry)(void) = (int (*)(void))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(), 87);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_inline_methods_jit)
{
    /* Impl methods defined INLINE (non-extern, with a body) are emitted as
       real Strata functions under their qualified symbol and called through
       the same `expr.Method(...)` / `Type.Method(...)` rewrite as extern
       methods. Covers: struct receivers, `ref self` mutation, static-style
       factories, receiver passing between methods, and recursion. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct Vec3 { float x; float y; float z; };\n"
        "impl Vec3 {\n"
        "    Vec3 Make(float x, float y, float z) { return { .x = x, .y = y, .z = z }; }\n"
        "    float LengthSq(Vec3 self) { return self.x * self.x + self.y * self.y + self.z * self.z; }\n"
        "    void Scale(ref Vec3 self, float s) { self.x = self.x * s; self.y = self.y * s; self.z = self.z * s; }\n"
        "    float Dot(Vec3 self, Vec3 other) { return self.x * other.x + self.y * other.y + self.z * other.z; }\n"
        "}\n"
        "struct Num { int v; };\n"
        "impl Num {\n"
        "    int Fact(Num self, int n) { if (n <= 1) { return 1; } return self.v * self.Fact(n - 1); }\n"
        "}\n"
        "int entry()\n"
        "{\n"
        "  Vec3 v = Vec3.Make(3.0, 4.0, 0.0);\n"
        "  float l = v.LengthSq();\n"
        "  v.Scale(2.0);\n"
        "  Vec3 w = Vec3.Make(1.0, 1.0, 1.0);\n"
        "  if (l != 25.0) { return 1; }\n"              /* 3^2 + 4^2 */
        "  if (v.LengthSq() != 100.0) { return 2; }\n" /* scaled: 6^2 + 8^2 */
        "  if (v.Dot(w) != 14.0) { return 3; }\n"      /* 6 + 8 + 0 */
        "  Num n = { .v = 2 };\n"
        "  if (n.Fact(5) != 16) { return 4; }\n"      /* recursive inline method: 2^4 */
        "  return 0;\n"
        "}\n",
        "inline_impl", &err);
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
        STRATA_CHECK_EQ(entry(), 0);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}
typedef struct
{
    int count;
} HostWidget;

static void HostWidgetGetCount(void* self, int* out)
{
    *out = ((HostWidget*)self)->count;
}

static void HostWidgetSetCount(void* self, int v)
{
    ((HostWidget*)self)->count = v;
}

typedef struct
{
    float x, y, z;
} HostVec3;

static void HostWidgetGetPos(void* self, HostVec3* out)
{
    out->x = 1;
    out->y = 2;
    out->z = 3;
}

STRATA_TEST(impl_property_getter_return_param_jit)
{
    /* A property getter may use a `return` out-param: the host writes the
       value through the out pointer instead of returning it. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "handle Widget;\n"
        "impl Widget {\n"
        "    extern void GetCount(Widget self, return int c);\n"
        "    extern void SetCount(Widget self, int v);\n"
        "    property int Count { get = Widget_GetCount; set = Widget_SetCount; }\n"
        "}\n"
        "int entry(Widget w)\n"
        "{\n"
        "  int c = w.Count;\n"
        "  w.Count = c + 1;\n"
        "  return w.Count;\n"
        "}\n",
        "prop_return_param", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "Widget_GetCount", (void*)&HostWidgetGetCount));
    STRATA_CHECK(strataJitAddSymbol(jit, "Widget_SetCount", (void*)&HostWidgetSetCount));

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        HostWidget w = { 5 };
        STRATA_CHECK_EQ(entry(&w), 6); /* read 5, write 6, read 6 */
        STRATA_CHECK_EQ(w.count, 6);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_property_getter_return_param_struct_jit)
{
    /* A struct-typed property via a `return` out-param: externs can't return
       a struct by value, but the out-param crosses the whole struct through
       a pointer. */
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c,
        "struct Vec3 { float x; float y; float z; };\n"
        "handle Widget;\n"
        "impl Widget {\n"
        "    extern void GetPos(Widget self, return Vec3 p);\n"
        "    property Vec3 Pos { get = Widget_GetPos; }\n"
        "}\n"
        "int entry(Widget w)\n"
        "{\n"
        "  Vec3 p = w.Pos;\n"
        "  return (int)(p.x + p.y + p.z);\n"
        "}\n",
        "prop_return_param_struct", &err);
    STRATA_CHECK(jit != NULL);
    if (!jit)
    {
        printf("  JIT failed: %s\n", err ? err : "(none)");
        strataFree((char*)err);
        strataCompilerDestroy(c);
        return;
    }

    STRATA_CHECK(strataJitAddSymbol(jit, "Widget_GetPos", (void*)&HostWidgetGetPos));

    int (*entry)(void*) = (int (*)(void*))strataJitGetFunction(jit, "entry");
    STRATA_CHECK(entry != NULL);
    if (entry)
    {
        STRATA_CHECK_EQ(entry(NULL), 6); /* 1 + 2 + 3 */
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(impl_property_setter_return_param_rejected)
{
    /* A setter must take (self, value); a `return` out-param in that slot
       leaves no value param, so it is rejected. */
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    ParseAndResolve("handle Widget;\n"
                    "impl Widget {\n"
                    "    extern void SetCount(Widget self, return int v);\n"
                    "    property int Count { set = Widget_SetCount; }\n"
                    "}\n",
                    &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(Contains(ErrText(&diag, &arena), "must take (Widget self, value)"));
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

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
