#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"
#include "Sema/ResolveOverloads.h"
#include "Test.h"
#include "Util.h"
#include "strata/strata.h"

static const char* kClassifySource = "struct P { int x; };\n"
                                     "struct Holder { ^P p; };\n"
                                     "struct Named { string s; };\n"
                                     "struct Wrapped = Holder;\n"
                                     "struct Res { int id; }\n"
                                     "impl Res { void Drop(ref Res self) { } }\n"
                                     "struct HasRes { Res r; };\n";

static TypeProps Classify(const TypeRegistry* reg, Arena* arena, const char* spelling)
{
    TypeName t = TypeNameParse(arena, spelling);
    return TypeClassify(reg, arena, &t);
}

STRATA_TEST(type_classify_properties)
{
    Arena arena;
    arena_init(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(kClassifySource, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ResolveOverloads(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    TypeRegistry reg;
    TypeRegistryInit(&reg);
    TypeRegistryBuild(&reg, mod);

    TypeProps p = Classify(&reg, &arena, "int");
    STRATA_CHECK(p.trivial && !p.owning && !p.userDrop && p.zeroValid);

    p = Classify(&reg, &arena, "P");
    STRATA_CHECK(p.trivial && p.zeroValid);

    p = Classify(&reg, &arena, "string");
    STRATA_CHECK(p.owning && !p.trivial && !p.userDrop && p.zeroValid);

    p = Classify(&reg, &arena, "P[]");
    STRATA_CHECK(p.owning && p.zeroValid);

    p = Classify(&reg, &arena, "^P");
    STRATA_CHECK(p.owning && !p.zeroValid);

    p = Classify(&reg, &arena, "P?");
    STRATA_CHECK(p.owning && p.zeroValid);

    p = Classify(&reg, &arena, "Holder");
    STRATA_CHECK(p.owning && !p.zeroValid);

    p = Classify(&reg, &arena, "Wrapped");
    STRATA_CHECK(p.owning && !p.zeroValid);

    p = Classify(&reg, &arena, "Named");
    STRATA_CHECK(p.owning && !p.userDrop && p.zeroValid);

    p = Classify(&reg, &arena, "Res");
    STRATA_CHECK(p.owning && p.userDrop);

    p = Classify(&reg, &arena, "HasRes");
    STRATA_CHECK(p.owning && p.userDrop);

    TypeRegistryFree(&reg);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
