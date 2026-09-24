/* ComponentTests.c — `@component` structs, field default values, type metadata
 * (strata_types.h) and symbol prefixes. */

#include "Util.h"
#include "Test.h"
#include "strata/strata.h"
#include "strata/strata_types.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const char* kComponentSource = "handle Entity;\n"
                                      "enum Team : byte { Red = 1, Blue = 2 };\n"
                                      "struct Stats { float armor = 2.5; int level = 3; };\n"
                                      "@component\n"
                                      "struct Health\n"
                                      "{\n"
                                      "    float current = 100.0;\n"
                                      "    float maximum = 100.0 * 2.0;\n"
                                      "    bool invulnerable = true;\n"
                                      "    Team team = Team.Blue;\n"
                                      "    float3 tint = float3(0.5, 0.25, 1.0);\n"
                                      "    Stats stats;\n"
                                      "    Stats[2] history;\n"
                                      "    int[3] slots;\n"
                                      "    Entity owner;\n"
                                      "    long big = -5;\n"
                                      "};\n";

static char* ResolveDiagnostics(const char* src, Arena* arena, DiagnosticEngine* diag)
{
    ParseAndResolve(src, diag, arena);

    SourceManager sm;
    SourceManagerInit(&sm);

    return DiagFormat(diag, &sm, 1, arena);
}

static float ReadF32(const void* base, size_t offset)
{
    float value;
    memcpy(&value, (const uint8_t*)base + offset, sizeof(value));
    return value;
}

static int32_t ReadI32(const void* base, size_t offset)
{
    int32_t value;
    memcpy(&value, (const uint8_t*)base + offset, sizeof(value));
    return value;
}

static int64_t ReadI64(const void* base, size_t offset)
{
    int64_t value;
    memcpy(&value, (const uint8_t*)base + offset, sizeof(value));
    return value;
}

static int FindField(const StrataTypes* types, const StrataTypesStruct* owner, const char* name, StrataTypesField* out)
{
    for (uint32_t i = 0; i < owner->fieldCount; i++)
    {
        if (strataTypesGetField(types, owner, i, out) && strcmp(out->name, name) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int NearlyEqual(float a, float b)
{
    return fabsf(a - b) < 0.0001f;
}

/* -- Parsing ------------------------------------------------------------------------------ */

STRATA_TEST(component_attribute_parses)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    Module* mod = ParseModule("@component\n"
                              "struct Health { float current = 100.0; bool dead; };\n"
                              "struct Plain { int x; };\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 2);

    StructDecl* health = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(health->isComponent);
    STRATA_CHECK_EQ((long)health->attributes.count, 1);
    STRATA_CHECK(strcmp(((Attribute*)VecGet(&health->attributes, 0))->name, "component") == 0);

    FieldDecl* current = (FieldDecl*)VecGet(&health->fields, 0);
    FieldDecl* dead = (FieldDecl*)VecGet(&health->fields, 1);
    STRATA_CHECK(current->defaultValue != NULL);
    STRATA_CHECK(dead->defaultValue == NULL);

    StructDecl* plain = (StructDecl*)VecGet(&mod->structs, 1);
    STRATA_CHECK(!plain->isComponent);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(component_attribute_misuse_is_rejected)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    char* d = ResolveDiagnostics("@shiny struct A { int x; };\n"
                                 "@component struct B;\n"
                                 "@component extern struct C { int x; };\n"
                                 "@component struct D = int;\n"
                                 "@component handle E;\n",
                                 &arena, &diag);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(strstr(d, "unknown struct attribute '@shiny'") != NULL);
    STRATA_CHECK(strstr(d, "'@component' requires a struct definition with a body") != NULL);
    STRATA_CHECK(strstr(d, "attributes are only allowed on function and struct definitions") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(function_attributes_still_work)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    Module* mod = ParseModule("@private int helper() { return 1; }\n"
                              "@component int bad() { return 2; }\n",
                              &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(((FunctionDecl*)VecGet(&mod->functions, 0))->isPrivate);

    SourceManager sm;
    SourceManagerInit(&sm);
    char* d = DiagFormat(&diag, &sm, 1, &arena);
    STRATA_CHECK(strstr(d, "unknown attribute '@component'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* -- Component field rules ---------------------------------------------------------------- */

STRATA_TEST(component_accepts_plain_data)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    ResolveDiagnostics(kComponentSource, &arena, &diag);
    STRATA_CHECK(!DiagHasErrors(&diag));

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(component_rejects_owning_and_cstring_fields)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    char* d = ResolveDiagnostics("struct Named { string name; };\n"
                                 "@component struct Bad\n"
                                 "{\n"
                                 "    string label;\n"
                                 "    ^int boxed;\n"
                                 "    int? maybe;\n"
                                 "    int[] list;\n"
                                 "    cstring raw;\n"
                                 "    Named nested;\n"
                                 "};\n",
                                 &arena, &diag);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(strstr(d, "field 'label' has type 'string', which is a string") != NULL);
    STRATA_CHECK(strstr(d, "field 'boxed' has type '^int', which is an owning box") != NULL);
    STRATA_CHECK(strstr(d, "field 'maybe' has type 'int?', which is an optional") != NULL);
    STRATA_CHECK(strstr(d, "field 'list' has type 'int[]', which is a dynamic array") != NULL);
    STRATA_CHECK(strstr(d, "field 'raw' has type 'cstring', which is a cstring") != NULL);
    STRATA_CHECK(strstr(d, "field 'nested' has type 'Named', which contains 'string' (a string)") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

/* -- Field defaults ----------------------------------------------------------------------- */

STRATA_TEST(field_default_must_be_constant_and_typed)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    char* d = ResolveDiagnostics("int g_value = 3;\n"
                                 "const int kLimit = 7;\n"
                                 "struct S\n"
                                 "{\n"
                                 "    int fromGlobal = g_value;\n"
                                 "    int fromConst = kLimit * 2;\n"
                                 "    string text = \"hi\";\n"
                                 "    int fromVector = float2(1.0, 2.0);\n"
                                 "    float3 v = float3(1.0, 2.0);\n"
                                 "};\n",
                                 &arena, &diag);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(strstr(d, "default value for field 'fromGlobal' must be a compile-time constant") != NULL);
    STRATA_CHECK(strstr(d, "fromConst") == NULL);
    STRATA_CHECK(strstr(d, "field 'text' of type 'string' can't have a default value") != NULL);
    STRATA_CHECK(strstr(d, "default value for field 'fromVector' of type 'int' can't be of type 'float2'") != NULL);
    STRATA_CHECK(strstr(d, "default value for field 'v' must be a compile-time constant 'float3(...)'") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

static StrataJit* CompileJit(const char* src)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, src, "components", &err);

    if (err)
    {
        fprintf(stderr, "%s\n", err);
        strataFree((char*)err);
    }

    strataCompilerDestroy(c);

    return jit;
}

STRATA_TEST(jit_field_defaults_apply_to_literals_and_locals)
{
    char source[4096];
    snprintf(source, sizeof(source), "%s%s", kComponentSource,
             "float literal_sum() { Health h = Health { .current = 5.0 }; return h.current + h.maximum; }\n"
             "float local_sum() { Health h; return h.current + h.stats.armor + (float)h.history[1].level; }\n"
             "int flags() {\n"
             "    Health h;\n"
             "    int r = 0;\n"
             "    if (h.invulnerable) { r = r + 1; }\n"
             "    if (h.team == Team.Blue) { r = r + 10; }\n"
             "    if (h.big == -5) { r = r + 100; }\n"
             "    if (h.slots[2] == 0) { r = r + 1000; }\n"
             "    return r;\n"
             "}\n"
             "float tint_sum() { Health h; return h.tint.x + h.tint.y + h.tint.z; }\n"
             "float nested_literal() { Stats s = Stats { .level = 9 }; return s.armor + (float)s.level; }\n");

    StrataJit* jit = CompileJit(source);
    STRATA_CHECK(jit != NULL);

    if (!jit)
    {
        return;
    }

    float (*literalSum)(void) = (float (*)(void))strataJitGetFunction(jit, "literal_sum");
    float (*localSum)(void) = (float (*)(void))strataJitGetFunction(jit, "local_sum");
    int (*flags)(void) = (int (*)(void))strataJitGetFunction(jit, "flags");
    float (*tintSum)(void) = (float (*)(void))strataJitGetFunction(jit, "tint_sum");
    float (*nestedLiteral)(void) = (float (*)(void))strataJitGetFunction(jit, "nested_literal");

    STRATA_CHECK(literalSum && localSum && flags && tintSum && nestedLiteral);

    if (literalSum && localSum && flags && tintSum && nestedLiteral)
    {
        STRATA_CHECK(NearlyEqual(literalSum(), 205.0f));
        STRATA_CHECK(NearlyEqual(localSum(), 105.5f));
        STRATA_CHECK_EQ(flags(), 1111);
        STRATA_CHECK(NearlyEqual(tintSum(), 1.75f));
        STRATA_CHECK(NearlyEqual(nestedLiteral(), 11.5f));
    }

    strataJitDestroy(jit);
}

STRATA_TEST(jit_field_defaults_apply_to_globals)
{
    StrataJit* jit = CompileJit("struct Settings { int volume = 7; float gain = 0.5; };\n"
                                "Settings g_settings;\n"
                                "int volume() { return g_settings.volume; }\n");
    STRATA_CHECK(jit != NULL);

    if (!jit)
    {
        return;
    }

    STRATA_CHECK(strataJitHasContext(jit));

    void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
    void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
    int (*volume)(void*) = (int (*)(void*))strataJitGetFunction(jit, "volume");
    STRATA_CHECK(create && destroy && volume);

    if (create && destroy && volume)
    {
        void* context = create();
        STRATA_CHECK_EQ(volume(context), 7);
        destroy(context);
    }

    strataJitDestroy(jit);
}

/* -- Type metadata ------------------------------------------------------------------------ */

STRATA_TEST(type_metadata_describes_components)
{
    StrataCompiler* c = strataCompilerCreate();
    void* bytes = NULL;
    size_t size = 0;
    const char* err = NULL;

    STRATA_CHECK(strataCompileTypeMetadataString(c, kComponentSource, "components", &bytes, &size, &err));
    STRATA_CHECK(err == NULL);
    STRATA_CHECK(bytes != NULL);

    StrataTypes types;
    STRATA_CHECK(strataTypesOpen(&types, bytes, size));
    STRATA_CHECK_EQ(strataTypesStructCount(&types), 2);

    /* Dependency order: Stats (used by Health) comes first. */
    StrataTypesStruct stats;
    StrataTypesStruct health;
    STRATA_CHECK(strataTypesGetStruct(&types, 0, &stats));
    STRATA_CHECK(strataTypesGetStruct(&types, 1, &health));
    STRATA_CHECK(strcmp(stats.name, "Stats") == 0);
    STRATA_CHECK(strcmp(health.name, "Health") == 0);
    STRATA_CHECK(strcmp(health.moduleName, "components") == 0);
    STRATA_CHECK_EQ(stats.flags & STRATA_TYPES_STRUCT_COMPONENT, 0);
    STRATA_CHECK_EQ(health.flags & STRATA_TYPES_STRUCT_COMPONENT, STRATA_TYPES_STRUCT_COMPONENT);
    STRATA_CHECK(strataTypesHasAttribute(&types, &health, "component"));
    STRATA_CHECK_EQ(health.size, 96);
    STRATA_CHECK_EQ(health.alignment, 16);
    STRATA_CHECK_EQ(health.fieldCount, 10);

    StrataTypesField field;
    STRATA_CHECK(FindField(&types, &health, "tint", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_FLOAT3);
    STRATA_CHECK_EQ(field.offset, 16);
    STRATA_CHECK_EQ(field.size, 16);

    STRATA_CHECK(FindField(&types, &health, "team", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_ENUM);
    STRATA_CHECK_EQ(field.offset, 9);
    STRATA_CHECK_EQ(field.size, 1);
    STRATA_CHECK_EQ(field.typeIndex, 0);
    STRATA_CHECK(strcmp(field.typeName, "Team") == 0);

    STRATA_CHECK(FindField(&types, &health, "history", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_STRUCT);
    STRATA_CHECK_EQ(field.arrayLength, 2);
    STRATA_CHECK_EQ(field.typeIndex, 0);
    STRATA_CHECK_EQ(field.offset, 40);
    STRATA_CHECK_EQ(field.size, 16);

    STRATA_CHECK(FindField(&types, &health, "slots", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_I32);
    STRATA_CHECK_EQ(field.arrayLength, 3);
    STRATA_CHECK_EQ(field.size, 12);

    STRATA_CHECK(FindField(&types, &health, "owner", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_HANDLE);
    STRATA_CHECK_EQ(field.offset, 72);
    STRATA_CHECK(strcmp(field.typeName, "Entity") == 0);

    STRATA_CHECK(FindField(&types, &health, "big", &field));
    STRATA_CHECK_EQ(field.kind, STRATA_TYPE_I64);
    STRATA_CHECK_EQ(field.offset, 80);

    /* The default value is the struct with its field defaults (nested ones included). */
    const uint8_t* defaults = (const uint8_t*)health.defaultValue;
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 0), 100.0f));
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 4), 200.0f));
    STRATA_CHECK_EQ(defaults[8], 1);
    STRATA_CHECK_EQ(defaults[9], 2);
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 16), 0.5f));
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 20), 0.25f));
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 24), 1.0f));
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 32), 2.5f));
    STRATA_CHECK_EQ(ReadI32(defaults, 36), 3);
    STRATA_CHECK(NearlyEqual(ReadF32(defaults, 48), 2.5f));
    STRATA_CHECK_EQ(ReadI32(defaults, 52), 3);
    STRATA_CHECK_EQ(ReadI32(defaults, 56), 0);
    STRATA_CHECK_EQ(ReadI64(defaults, 72), 0);
    STRATA_CHECK_EQ(ReadI64(defaults, 80), -5);

    STRATA_CHECK_EQ(strataTypesEnumCount(&types), 1);
    StrataTypesEnum team;
    StrataTypesEnumValue value;
    STRATA_CHECK(strataTypesGetEnum(&types, 0, &team));
    STRATA_CHECK(strcmp(team.name, "Team") == 0);
    STRATA_CHECK_EQ(team.underlyingKind, STRATA_TYPE_U8);
    STRATA_CHECK_EQ(team.valueCount, 2);
    STRATA_CHECK(strataTypesGetEnumValue(&types, &team, 1, &value));
    STRATA_CHECK(strcmp(value.name, "Blue") == 0);
    STRATA_CHECK_EQ((long)value.value, 2);

    strataFreeTypeMetadata(bytes);
    strataCompilerDestroy(c);
}

STRATA_TEST(type_metadata_matches_between_jit_and_types_only)
{
    StrataCompiler* c = strataCompilerCreate();
    void* typesOnly = NULL;
    size_t typesOnlySize = 0;
    const char* err = NULL;
    STRATA_CHECK(strataCompileTypeMetadataString(c, kComponentSource, "components", &typesOnly, &typesOnlySize, &err));

    StrataJit* jit = strataJitCompileString(c, kComponentSource, "components", &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        size_t jitSize = 0;
        const void* jitBytes = strataJitGetTypeMetadata(jit, &jitSize);
        STRATA_CHECK(jitBytes != NULL);
        STRATA_CHECK_EQ((long)jitSize, (long)typesOnlySize);
        STRATA_CHECK(jitBytes && typesOnly && jitSize == typesOnlySize && memcmp(jitBytes, typesOnly, jitSize) == 0);

        strataJitDestroy(jit);
    }

    strataFreeTypeMetadata(typesOnly);
    strataCompilerDestroy(c);
}

STRATA_TEST(type_metadata_absent_without_components)
{
    StrataCompiler* c = strataCompilerCreate();
    void* bytes = (void*)1;
    size_t size = 1;
    const char* err = NULL;

    STRATA_CHECK(strataCompileTypeMetadataString(c, "struct S { int x = 1; };\nint f() { return 1; }\n", "plain",
                                                 &bytes, &size, &err));
    STRATA_CHECK(bytes == NULL);
    STRATA_CHECK_EQ((long)size, 0);

    StrataJit* jit = strataJitCompileString(c, "int f() { return 1; }\n", "plain", &err);
    STRATA_CHECK(jit != NULL);
    STRATA_CHECK(strataJitGetTypeMetadata(jit, &size) == NULL);
    strataJitDestroy(jit);

    strataCompilerDestroy(c);
}

STRATA_TEST(type_metadata_reports_compile_errors)
{
    StrataCompiler* c = strataCompilerCreate();
    void* bytes = NULL;
    size_t size = 0;
    const char* err = NULL;

    STRATA_CHECK(!strataCompileTypeMetadataString(c, "@component struct Bad { string name; };\n", "bad", &bytes, &size,
                                                  &err));
    STRATA_CHECK(err != NULL && strstr(err, "components may only hold plain data") != NULL);
    STRATA_CHECK(bytes == NULL);

    strataFree((char*)err);
    strataCompilerDestroy(c);
}

static uint64_t LayoutHashOf(const char* source)
{
    StrataCompiler* c = strataCompilerCreate();
    void* bytes = NULL;
    size_t size = 0;
    const char* err = NULL;
    uint64_t hash = 0;

    if (strataCompileTypeMetadataString(c, source, "hash", &bytes, &size, &err) && bytes)
    {
        StrataTypes types;
        StrataTypesStruct last;

        if (strataTypesOpen(&types, bytes, size)
            && strataTypesGetStruct(&types, strataTypesStructCount(&types) - 1, &last))
        {
            hash = last.layoutHash;
        }
    }

    strataFreeTypeMetadata(bytes);
    strataFree((char*)err);
    strataCompilerDestroy(c);

    return hash;
}

STRATA_TEST(type_metadata_layout_hash_tracks_layout_not_defaults)
{
    uint64_t original = LayoutHashOf("@component struct A { int x = 1; float y; };\n");
    uint64_t newDefault = LayoutHashOf("@component struct A { int x = 5; float y = 2.0; };\n");
    uint64_t newField = LayoutHashOf("@component struct A { int x = 1; float y; bool z; };\n");
    uint64_t renamed = LayoutHashOf("@component struct A { int count = 1; float y; };\n");
    uint64_t nestedChange = LayoutHashOf("struct B { int q; long r; };\n@component struct A { B b; };\n");
    uint64_t nestedOriginal = LayoutHashOf("struct B { int q; };\n@component struct A { B b; };\n");

    STRATA_CHECK(original != 0);
    STRATA_CHECK(original == newDefault);
    STRATA_CHECK(original != newField);
    STRATA_CHECK(original != renamed);
    STRATA_CHECK(nestedChange != nestedOriginal);
}

STRATA_TEST(type_metadata_reader_rejects_bad_blobs)
{
    StrataCompiler* c = strataCompilerCreate();
    void* bytes = NULL;
    size_t size = 0;
    const char* err = NULL;
    STRATA_CHECK(strataCompileTypeMetadataString(c, kComponentSource, "components", &bytes, &size, &err));

    StrataTypes types;
    STRATA_CHECK(!strataTypesOpen(&types, NULL, 0));
    STRATA_CHECK(!strataTypesOpen(&types, bytes, 16));
    STRATA_CHECK(!strataTypesOpen(&types, bytes, size - 1));

    uint8_t* copy = (uint8_t*)malloc(size);
    memcpy(copy, bytes, size);

    copy[0] ^= 0xFF;
    STRATA_CHECK(!strataTypesOpen(&types, copy, size));
    copy[0] ^= 0xFF;

    copy[4] = 99; /* version */
    STRATA_CHECK(!strataTypesOpen(&types, copy, size));
    copy[4] = (uint8_t)STRATA_TYPES_VERSION;

    copy[16 + 8] = 0xFF; /* struct table offset past the end */
    copy[16 + 9] = 0xFF;
    STRATA_CHECK(!strataTypesOpen(&types, copy, size));

    free(copy);
    strataFreeTypeMetadata(bytes);
    strataCompilerDestroy(c);
}

/* -- Generic externs ---------------------------------------------------------------------- */

static uint8_t s_hostStore[64];
static int s_hostHasValue;
static const StrataTypeDesc* s_hostLastType;
static void* s_hostLastEntity;

static bool HostGetComponent(void* entity, void* value, const StrataTypeDesc* type)
{
    s_hostLastEntity = entity;
    s_hostLastType = type;

    if (!s_hostHasValue || type->size > sizeof(s_hostStore))
    {
        return false;
    }

    memcpy(value, s_hostStore, type->size);
    return true;
}

static void HostSetComponent(void* entity, void* value, const StrataTypeDesc* type)
{
    s_hostLastEntity = entity;
    s_hostLastType = type;

    if (type->size <= sizeof(s_hostStore))
    {
        memcpy(s_hostStore, value, type->size);
        s_hostHasValue = 1;
    }
}

static bool HostHasComponent(void* entity, const StrataTypeDesc* type)
{
    s_hostLastEntity = entity;
    s_hostLastType = type;
    return s_hostHasValue != 0;
}

static const char* kGenericExternSource = "handle Entity;\n"
                                          "@component struct Health { float current = 100.0; float maximum = 150.0; };\n"
                                          "@component struct Armor { int value = 7; };\n"
                                          "extern<T> bool GetComponent(Entity entity, ref T value);\n"
                                          "extern<T> void SetComponent(Entity entity, T value);\n"
                                          "extern<T> bool HasComponent(Entity entity);\n"
                                          "float roundtrip(Entity e)\n"
                                          "{\n"
                                          "    Health h;\n"
                                          "    h.current = 42.0;\n"
                                          "    SetComponent(e, h);\n"
                                          "    Health read;\n"
                                          "    read.current = 0.0;\n"
                                          "    if (!GetComponent(e, read)) { return -1.0; }\n"
                                          "    if (!HasComponent<Health>(e)) { return -2.0; }\n"
                                          "    return read.current + read.maximum;\n"
                                          "}\n"
                                          "int armor_value(Entity e)\n"
                                          "{\n"
                                          "    Armor a;\n"
                                          "    SetComponent<Armor>(e, a);\n"
                                          "    Armor b;\n"
                                          "    b.value = 0;\n"
                                          "    GetComponent(e, b);\n"
                                          "    return b.value;\n"
                                          "}\n";

STRATA_TEST(generic_extern_calls_reach_one_host_function)
{
    StrataCompiler* c = strataCompilerCreate();
    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, kGenericExternSource, "generic", &err);
    STRATA_CHECK(jit != NULL);

    if (err)
    {
        fprintf(stderr, "%s\n", err);
        strataFree((char*)err);
    }

    if (!jit)
    {
        strataCompilerDestroy(c);
        return;
    }

    /* The host sees each generic once, whatever types the script used. */
    size_t getCount = 0;

    for (size_t i = 0; i < strataJitGetExternSymbolCount(jit); i++)
    {
        getCount += strcmp(strataJitGetExternSymbolName(jit, i), "GetComponent") == 0;
    }

    STRATA_CHECK_EQ((long)getCount, 1);

    STRATA_CHECK(strataJitAddSymbol(jit, "GetComponent", (void*)&HostGetComponent));
    STRATA_CHECK(strataJitAddSymbol(jit, "SetComponent", (void*)&HostSetComponent));
    STRATA_CHECK(strataJitAddSymbol(jit, "HasComponent", (void*)&HostHasComponent));

    float (*roundtrip)(void*) = (float (*)(void*))strataJitGetFunction(jit, "roundtrip");
    int (*armorValue)(void*) = (int (*)(void*))strataJitGetFunction(jit, "armor_value");
    STRATA_CHECK(roundtrip && armorValue);

    void* entity = (void*)(uintptr_t)0x1234;

    if (roundtrip && armorValue)
    {
        s_hostHasValue = 0;
        STRATA_CHECK(NearlyEqual(roundtrip(entity), 192.0f));
        STRATA_CHECK(s_hostLastEntity == entity);
        STRATA_CHECK(s_hostLastType != NULL);

        if (s_hostLastType)
        {
            STRATA_CHECK(strcmp(strataTypeDescName(s_hostLastType), "Health") == 0);
            STRATA_CHECK_EQ(s_hostLastType->size, 8);
            STRATA_CHECK_EQ(s_hostLastType->alignment, 4);
        }

        /* The descriptor's layout hash matches the metadata's for the same struct. */
        const StrataTypeDesc* healthType = s_hostLastType;
        size_t metadataSize = 0;
        const void* metadata = strataJitGetTypeMetadata(jit, &metadataSize);
        StrataTypes types;
        StrataTypesStruct metadataStruct;
        int matchedHash = 0;
        STRATA_CHECK(strataTypesOpen(&types, metadata, metadataSize));

        for (uint32_t i = 0; i < strataTypesStructCount(&types); i++)
        {
            if (strataTypesGetStruct(&types, i, &metadataStruct) && strcmp(metadataStruct.name, "Health") == 0)
            {
                matchedHash = healthType && healthType->layoutHash == metadataStruct.layoutHash;
            }
        }

        STRATA_CHECK(matchedHash);

        STRATA_CHECK_EQ(armorValue(entity), 7);
        STRATA_CHECK(s_hostLastType && strcmp(strataTypeDescName(s_hostLastType), "Armor") == 0);
        STRATA_CHECK(healthType != s_hostLastType);
    }

    strataJitDestroy(jit);
    strataCompilerDestroy(c);
}

STRATA_TEST(generic_extern_rules)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    char* d = ResolveDiagnostics("handle Entity;\n"
                                 "struct Health { float current; };\n"
                                 "struct Named { string name; };\n"
                                 "extern<T> T Make();\n"
                                 "extern<T> void Boxed(^T value);\n"
                                 "extern<T> bool Has(Entity entity);\n"
                                 "extern<T> bool Get(Entity entity, ref T value);\n"
                                 "int plain(int x) { return x; }\n"
                                 "void run(Entity e)\n"
                                 "{\n"
                                 "    Has(e);\n"
                                 "    Has<Named>(e);\n"
                                 "    Has<int>(e);\n"
                                 "    plain<Health>(1);\n"
                                 "    Get(e, Health { .current = 1.0 });\n"
                                 "}\n",
                                 &arena, &diag);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(strstr(d, "generic extern 'Make' can't return its type parameter 'T'") != NULL);
    STRATA_CHECK(strstr(d, "parameter 'value' of generic extern 'Boxed' must be 'T' or 'ref T', not '^T'") != NULL);
    STRATA_CHECK(strstr(d, "can't infer 'T' for 'Has'; name it explicitly: 'Has<Type>(...)'") != NULL);
    STRATA_CHECK(strstr(d, "type argument 'Named' for 'Has' must be plain data") != NULL);
    STRATA_CHECK(strstr(d, "type argument 'int' for 'Has' must be a struct type") != NULL);
    STRATA_CHECK(strstr(d, "'plain' is not a generic extern; it takes no type argument") != NULL);
    STRATA_CHECK(strstr(d, "argument 2 of 'Get' is passed by 'ref' and must be a variable") != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(less_than_still_parses_as_comparison)
{
    StrataJit* jit = CompileJit("int pick(int a, int b, int c) { if (a < b) { return c; } return 0; }\n"
                                "int chained(int a, int b) { bool r = a < b; if (r) { return 1; } return 2; }\n");
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        int (*pick)(int, int, int) = (int (*)(int, int, int))strataJitGetFunction(jit, "pick");
        int (*chained)(int, int) = (int (*)(int, int))strataJitGetFunction(jit, "chained");
        STRATA_CHECK(pick && chained);

        if (pick && chained)
        {
            STRATA_CHECK_EQ(pick(1, 2, 9), 9);
            STRATA_CHECK_EQ(chained(3, 2), 2);
        }

        strataJitDestroy(jit);
    }
}

/* -- Symbol prefixes ---------------------------------------------------------------------- */

STRATA_TEST(symbol_prefix_validates)
{
    StrataCompiler* c = strataCompilerCreate();

    STRATA_CHECK(strataSetSymbolPrefix(c, "Gun_"));
    STRATA_CHECK(strataSetSymbolPrefix(c, NULL));
    STRATA_CHECK(!strataSetSymbolPrefix(c, "bad-prefix"));
    STRATA_CHECK(!strataSetSymbolPrefix(c, "has space"));

    strataCompilerDestroy(c);
}

STRATA_TEST(symbol_prefix_jit_lookups_use_bare_names)
{
    StrataCompiler* c = strataCompilerCreate();
    STRATA_CHECK(strataSetSymbolPrefix(c, "Mod_"));

    const char* err = NULL;
    StrataJit* jit = strataJitCompileString(c, "int g_bonus = 4;\n"
                                               "int answer() { return 38 + g_bonus; }\n",
                                            "prefixed", &err);
    STRATA_CHECK(jit != NULL);

    if (jit)
    {
        void* (*create)(void) = (void* (*)(void))strataJitGetFunction(jit, "__strata_context_create");
        void (*destroy)(void*) = (void (*)(void*))strataJitGetFunction(jit, "__strata_context_destroy");
        int (*answer)(void*) = (int (*)(void*))strataJitGetFunction(jit, "answer");

        STRATA_CHECK(create && destroy && answer);
        STRATA_CHECK(strataJitGetFunction(jit, "Mod_answer") == (void*)answer);

        if (create && destroy && answer)
        {
            void* context = create();
            STRATA_CHECK_EQ(answer(context), 42);
            destroy(context);
        }

        strataJitDestroy(jit);
    }

    strataCompilerDestroy(c);
}
