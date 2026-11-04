#include "AST/ASTClone.h"
#include "Codegen/CodegenBackend.h"
#include "Sema/Monomorphize.h"
#include "Test.h"
#include "Util.h"

#include <stdlib.h>
#include <string.h>

static size_t DiagsContaining(const DiagnosticEngine* diag, const char* needle)
{
    size_t n = 0;

    for (size_t i = 0; i < diag->count; i++)
    {
        if (strstr(diag->diagnostics[i].message, needle))
        {
            n++;
        }
    }

    return n;
}

// Statement `i` of the body of the first module function.
static Node* FirstBodyStmt(Module* m, size_t i)
{
    FunctionDecl* fn = (FunctionDecl*)VecGet(&m->functions, 0);
    Block* block = (Block*)fn->body;

    return (Node*)VecGet(&block->statements, i);
}

// The initializer of `T x = init;` statement `i` of the first function.
static Node* InitOf(Module* m, size_t i)
{
    return ((VarDeclStmt*)FirstBodyStmt(m, i))->init;
}

// The type of `T x;` statement `i` of the first function.
static const TypeName* DeclTypeOf(Module* m, size_t i)
{
    return &((VarDeclStmt*)FirstBodyStmt(m, i))->type;
}

static const TypeName* TypeArg(const TypeName* t, size_t i)
{
    return &t->typeArgs[i];
}

STRATA_TEST(generic_struct_declares_type_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct pair<A, B> { A first; B second; }\nstruct number<T> { T value; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 2);

    StructDecl* pair = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(strcmp(pair->name, "pair") == 0);
    STRATA_CHECK_EQ((long)pair->genericParams.count, 2);
    STRATA_CHECK(strcmp((char*)VecGet(&pair->genericParams, 0), "A") == 0);
    STRATA_CHECK(strcmp((char*)VecGet(&pair->genericParams, 1), "B") == 0);

    StructDecl* number = (StructDecl*)VecGet(&mod->structs, 1);
    STRATA_CHECK_EQ((long)number->genericParams.count, 1);
    FieldDecl* field = (FieldDecl*)VecGet(&number->fields, 0);
    STRATA_CHECK(strcmp(field->type.name, "T") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_union_declares_type_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("union either<L, R> { L left; R right; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StructDecl* sd = (StructDecl*)VecGet(&mod->structs, 0);
    STRATA_CHECK(sd->isUnion);
    STRATA_CHECK_EQ((long)sd->genericParams.count, 2);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_impl_declares_type_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct number<T> { T value; }\n"
                              "impl number<T> { T get_value(const ref number<T> self) { return self.value; } }",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->impls.count, 1);

    ImplDecl* impl = (ImplDecl*)VecGet(&mod->impls, 0);
    STRATA_CHECK(strcmp(impl->handleName, "number") == 0);
    STRATA_CHECK_EQ((long)impl->genericParams.count, 1);
    STRATA_CHECK(strcmp((char*)VecGet(&impl->genericParams, 0), "T") == 0);

    FunctionDecl* method = (FunctionDecl*)VecGet(&impl->methods, 0);
    STRATA_CHECK(strcmp(method->name, "number_get_value") == 0);
    STRATA_CHECK_EQ((long)method->genericParams.count, 0);
    ParamDecl* self = (ParamDecl*)VecGet(&method->params, 0);
    STRATA_CHECK(strcmp(self->type.name, "number<T>") == 0);
    STRATA_CHECK(strcmp(self->type.genericBase, "number") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_function_declares_type_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct number<T> { T value; }\n"
                              "number<T> make_number<T>(T value) { number<T> num; return num; }",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->functions.count, 1);

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(strcmp(fn->name, "make_number") == 0);
    STRATA_CHECK_EQ((long)fn->genericParams.count, 1);
    STRATA_CHECK(strcmp(fn->returnType.name, "number<T>") == 0);
    STRATA_CHECK(strcmp(((ParamDecl*)VecGet(&fn->params, 0))->type.name, "T") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_nested_type_splits_shift_token)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { number<number<int>> x; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const TypeName* t = DeclTypeOf(mod, 0);
    STRATA_CHECK(strcmp(t->name, "number<number<int>>") == 0);
    STRATA_CHECK(strcmp(t->genericBase, "number") == 0);
    STRATA_CHECK_EQ((long)t->typeArgCount, 1);

    const TypeName* inner = TypeArg(t, 0);
    STRATA_CHECK(strcmp(inner->name, "number<int>") == 0);
    STRATA_CHECK_EQ((long)inner->typeArgCount, 1);
    STRATA_CHECK(strcmp(TypeArg(inner, 0)->name, "int") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_nested_type_closes_with_three_levels)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { a<b<c<int>>> x; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const TypeName* t = DeclTypeOf(mod, 0);
    STRATA_CHECK(strcmp(t->name, "a<b<c<int>>>") == 0);
    STRATA_CHECK(strcmp(TypeArg(TypeArg(t, 0), 0)->name, "c<int>") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_wrappers_around_and_inside_arguments)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct Foo { int a; }\n"
                              "int f() {\n"
                              "    number<^Foo> a;\n"
                              "    number<int[]> b;\n"
                              "    number<int>? c;\n"
                              "    number<int>[4] d;\n"
                              "    ^number<int> e;\n"
                              "    number<Foo?> g;\n"
                              "    return 0;\n"
                              "}",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const TypeName* a = DeclTypeOf(mod, 0);
    STRATA_CHECK(strcmp(a->name, "number<^Foo>") == 0);
    STRATA_CHECK(TypeArg(a, 0)->isBox);
    STRATA_CHECK(strcmp(TypeArg(a, 0)->name, "^Foo") == 0);

    const TypeName* b = DeclTypeOf(mod, 1);
    STRATA_CHECK(strcmp(b->name, "number<int[]>") == 0);
    STRATA_CHECK(TypeArg(b, 0)->isArray);
    STRATA_CHECK_EQ((long)TypeArg(b, 0)->length, -1);

    const TypeName* c = DeclTypeOf(mod, 2);
    STRATA_CHECK(c->isOptional);
    STRATA_CHECK(strcmp(c->name, "number<int>?") == 0);
    STRATA_CHECK(strcmp(c->inner->name, "number<int>") == 0);
    STRATA_CHECK_EQ((long)c->inner->typeArgCount, 1);

    const TypeName* d = DeclTypeOf(mod, 3);
    STRATA_CHECK(d->isArray);
    STRATA_CHECK_EQ((long)d->length, 4);
    STRATA_CHECK(strcmp(d->name, "number<int>[4]") == 0);
    STRATA_CHECK(strcmp(d->elem->name, "number<int>") == 0);

    const TypeName* e = DeclTypeOf(mod, 4);
    STRATA_CHECK(e->isBox);
    STRATA_CHECK(strcmp(e->name, "^number<int>") == 0);
    STRATA_CHECK(strcmp(e->inner->name, "number<int>") == 0);

    const TypeName* g = DeclTypeOf(mod, 5);
    STRATA_CHECK(strcmp(g->name, "number<Foo?>") == 0);
    STRATA_CHECK(TypeArg(g, 0)->isOptional);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_multi_argument_type_list)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { pair<int, float> p; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const TypeName* p = DeclTypeOf(mod, 0);
    STRATA_CHECK(strcmp(p->name, "pair<int,float>") == 0);
    STRATA_CHECK_EQ((long)p->typeArgCount, 2);
    STRATA_CHECK(strcmp(TypeArg(p, 0)->name, "int") == 0);
    STRATA_CHECK(strcmp(TypeArg(p, 1)->name, "float") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_call_single_type_argument_sets_type_arg)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return make_number<int>(3); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CallExpr* call = (CallExpr*)((ReturnStmt*)FirstBodyStmt(mod, 0))->value;
    STRATA_CHECK(call->base.kind == NodeCall);
    STRATA_CHECK(strcmp(call->callee, "make_number") == 0);
    STRATA_CHECK_EQ((long)call->typeArgCount, 1);
    STRATA_CHECK(call->typeArg == &call->typeArgs[0]);
    STRATA_CHECK(strcmp(call->typeArg->name, "int") == 0);
    STRATA_CHECK_EQ((long)call->args.count, 1);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_call_multiple_type_arguments)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return pair_of<int, float>(1, 2.0); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CallExpr* call = (CallExpr*)((ReturnStmt*)FirstBodyStmt(mod, 0))->value;
    STRATA_CHECK(strcmp(call->callee, "pair_of") == 0);
    STRATA_CHECK_EQ((long)call->typeArgCount, 2);
    STRATA_CHECK(call->typeArg == NULL);
    STRATA_CHECK(strcmp(call->typeArgs[1].name, "float") == 0);
    STRATA_CHECK_EQ((long)call->args.count, 2);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_call_with_nested_type_argument)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return wrap<number<int>>(1); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    CallExpr* call = (CallExpr*)((ReturnStmt*)FirstBodyStmt(mod, 0))->value;
    STRATA_CHECK(call->base.kind == NodeCall);
    STRATA_CHECK(strcmp(call->typeArg->name, "number<int>") == 0);
    STRATA_CHECK(strcmp(call->typeArg->genericBase, "number") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_struct_literal_with_type_arguments)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { number<float> n = number<float> { .value = 1.5 }; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StructInitExpr* init = (StructInitExpr*)InitOf(mod, 0);
    STRATA_CHECK(init->base.kind == NodeStructInit);
    STRATA_CHECK(strcmp(init->typeName, "number<float>") == 0);
    STRATA_CHECK(init->genericType != NULL);
    STRATA_CHECK(strcmp(init->genericType->name, "number<float>") == 0);
    STRATA_CHECK_EQ((long)init->genericType->typeArgCount, 1);
    STRATA_CHECK_EQ((long)init->fields.count, 1);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_comparisons_still_parse_as_comparisons)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f(int a, int b, int c, int d) {\n"
                              "    bool x = a < b;\n"
                              "    bool y = a < b && c > d;\n"
                              "    bool z = a < b >> c;\n"
                              "    g(a < b, c > d);\n"
                              "    if (a < b) { return 1; }\n"
                              "    return 0;\n"
                              "}",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    BinaryExpr* x = (BinaryExpr*)InitOf(mod, 0);
    STRATA_CHECK(x->base.kind == NodeBinary && x->op == BinLt);

    BinaryExpr* y = (BinaryExpr*)InitOf(mod, 1);
    STRATA_CHECK(y->base.kind == NodeBinary && y->op == BinLogicAnd);
    STRATA_CHECK(((BinaryExpr*)y->lhs)->op == BinLt);
    STRATA_CHECK(((BinaryExpr*)y->rhs)->op == BinGt);

    BinaryExpr* z = (BinaryExpr*)InitOf(mod, 2);
    STRATA_CHECK(z->base.kind == NodeBinary && z->op == BinLt);
    STRATA_CHECK(((BinaryExpr*)z->rhs)->op == BinShr);

    CallExpr* call = (CallExpr*)((ExprStmt*)FirstBodyStmt(mod, 3))->expr;
    STRATA_CHECK(call->base.kind == NodeCall);
    STRATA_CHECK_EQ((long)call->typeArgCount, 0);
    STRATA_CHECK(call->typeArg == NULL);
    STRATA_CHECK_EQ((long)call->args.count, 2);
    STRATA_CHECK(((BinaryExpr*)VecGet(&call->args, 0))->op == BinLt);
    STRATA_CHECK(((BinaryExpr*)VecGet(&call->args, 1))->op == BinGt);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_rejected_declarations_report_errors)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"struct s<int> { int x; }",                                      "builtin type"                                     },
        {"struct s<T, T> { int x; }",                                     "duplicate type parameter 'T'"                     },
        {"struct s<> { int x; }",                                         "expected a type parameter name"                   },
        {"struct s<T> = int;",                                            "a struct alias cannot have type parameters"       },
        {"extern struct s<T> { int x; }",                                 "an extern struct cannot have type parameters"     },
        {"struct s<T>;",                                                  "a forward declaration cannot have type parameters"},
        {"@component struct s<T> { int x; }",                             "'@component' cannot have type parameters"         },
        {"extern int f<T>(T x);",                                         "cannot have type parameters"                      },
        {"@export int f<T>(T x) { return 0; }",                           "cannot have type parameters"                      },
        {"int g<T> = 3;",                                                 "expected '(' after type parameters"               },
        {"struct H { int a; }\nimpl H { int M<T>(int x) { return 0; } }", "type parameters go on the impl"                   },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Arena arena;
        ArenaInit(&arena, 0);
        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);
        Module* mod = ParseModule(cases[i].src, &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        STRATA_CHECK(DiagsContaining(&diag, cases[i].needle) >= 1);
        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        ArenaFree(&arena);
    }
}

STRATA_TEST(generic_comparison_reports_no_diagnostics)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f(int a, int b) { return a < b; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)DiagCount(&diag), 0);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_ast_dump_prints_type_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct pair<A, B> { A first; B second; }\n"
                              "impl pair<A, B> { A get(const ref pair<A, B> self) { return self.first; } }\n"
                              "A first_of<A, B>(A a) { return a; }",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    char* dump = DumpAst(mod, &arena);
    STRATA_CHECK(strstr(dump, "struct pair<A, B> {") != NULL);
    STRATA_CHECK(strstr(dump, "impl pair<A, B> {") != NULL);
    STRATA_CHECK(strstr(dump, "A first_of<A, B>(") != NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

// A generic struct and a generic function with each wrapper around the parameter `T`, and a body that
// names `T` in a struct literal, a call, an array literal and a member access.
static const char kCloneSource[] = "struct number<T> { T value; int count = 3; }\n"
                                   "T pick<T>(const T x, ^T boxed, T[] items, T[4] slots, number<T> h, T? maybe, "
                                   "*T ptr, T[2][6] grid, T[SIZE] named)\n"
                                   "{\n"
                                   "    number<T> local = number<T> { .value = x };\n"
                                   "    T copy = pass<T>(x);\n"
                                   "    T[4] zeros = { x; 4 };\n"
                                   "    return T.max;\n"
                                   "}";

// True when `t` is set and its canonical spelling is `name`.
static bool NameIs(const TypeName* t, const char* name)
{
    return t && t->name && strcmp(t->name, name) == 0;
}

// True when `n` is an identifier spelled `name`.
static bool IdentIs(const Node* n, const char* name)
{
    return n && n->kind == NodeIdent && strcmp(((const IdentExpr*)n)->name, name) == 0;
}

// Statement `i` of the body of `fn`, a function template or a clone of one.
static Node* CloneBodyStmt(const FunctionDecl* fn, size_t i)
{
    return (Node*)VecGet(&((const Block*)fn->body)->statements, i);
}

static const ParamDecl* ParamOf(const FunctionDecl* fn, size_t i)
{
    return (const ParamDecl*)VecGet(&fn->params, i);
}

// Clones `n` under `s`, checks the copy has the same kind, and disposes it. An impl's method copies are
// disposed too, since AstDispose of an impl frees only its lists.
static void CloneAndDispose(Arena* arena, Node* n, const TypeSubst* s)
{
    Node* copy = AstCloneSubst(arena, n, s);
    STRATA_CHECK(copy != NULL);

    if (!copy)
    {
        return;
    }

    STRATA_CHECK(copy->kind == n->kind);

    if (copy->kind == NodeImpl)
    {
        ImplDecl* impl = (ImplDecl*)copy;

        for (size_t i = 0; i < impl->methods.count; i++)
        {
            AstDispose((Node*)VecGet(&impl->methods, i));
        }
    }

    AstDispose(copy);
}

STRATA_TEST(generic_clone_substitutes_parameter_types)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(kCloneSource, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* tpl = (FunctionDecl*)VecGet(&mod->functions, 0);
    TypeName args[1] = {TypeNameLeaf("int")};
    TypeSubst s = {&tpl->genericParams, args};
    FunctionDecl* fn = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, &s);
    STRATA_CHECK(fn != NULL && fn->base.kind == NodeFunction);

    // Each parameter type: its wrapper, and the substituted leaf inside it.
    STRATA_CHECK(NameIs(&fn->returnType, "int"));
    STRATA_CHECK(fn->returnType.primitiveType == GetPrimitiveType("int"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 0)->type, "int"));
    STRATA_CHECK(ParamOf(fn, 0)->type.isConst);
    STRATA_CHECK(NameIs(&ParamOf(fn, 1)->type, "^int"));
    STRATA_CHECK(ParamOf(fn, 1)->type.isBox && NameIs(ParamOf(fn, 1)->type.inner, "int"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 2)->type, "int[]"));
    STRATA_CHECK(ParamOf(fn, 2)->type.isArray && ParamOf(fn, 2)->type.length == -1);
    STRATA_CHECK(NameIs(&ParamOf(fn, 3)->type, "int[4]"));
    STRATA_CHECK(ParamOf(fn, 3)->type.isArray && ParamOf(fn, 3)->type.length == 4);
    STRATA_CHECK(NameIs(&ParamOf(fn, 4)->type, "number<int>"));
    STRATA_CHECK(strcmp(ParamOf(fn, 4)->type.genericBase, "number") == 0);
    STRATA_CHECK(NameIs(TypeArg(&ParamOf(fn, 4)->type, 0), "int"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 5)->type, "int?"));
    STRATA_CHECK(ParamOf(fn, 5)->type.isOptional && NameIs(ParamOf(fn, 5)->type.inner, "int"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 6)->type, "*int"));
    STRATA_CHECK(ParamOf(fn, 6)->type.isPointer && NameIs(ParamOf(fn, 6)->type.inner, "int"));
    // Two dimensions keep their source order, and a `[constName]` length stays for sema to resolve.
    STRATA_CHECK(NameIs(&ParamOf(fn, 7)->type, "int[2][6]"));
    STRATA_CHECK(NameIs(ParamOf(fn, 7)->type.elem, "int[6]"));
    STRATA_CHECK(ParamOf(fn, 7)->type.length == 2 && ParamOf(fn, 7)->type.elem->length == 6);
    STRATA_CHECK(NameIs(&ParamOf(fn, 8)->type, "int[SIZE]"));
    STRATA_CHECK(strcmp(ParamOf(fn, 8)->type.lengthName, "SIZE") == 0);

    // The body: a generic struct literal, a call's type argument, an array literal, and `T.max`.
    VarDeclStmt* local = (VarDeclStmt*)CloneBodyStmt(fn, 0);
    STRATA_CHECK(NameIs(&local->type, "number<int>"));
    StructInitExpr* init = (StructInitExpr*)local->init;
    STRATA_CHECK(strcmp(init->typeName, "number<int>") == 0);
    STRATA_CHECK(NameIs(init->genericType, "number<int>"));
    STRATA_CHECK(IdentIs(((StructInitField*)VecGet(&init->fields, 0))->value, "x"));

    VarDeclStmt* copy = (VarDeclStmt*)CloneBodyStmt(fn, 1);
    CallExpr* call = (CallExpr*)copy->init;
    STRATA_CHECK(NameIs(&copy->type, "int"));
    STRATA_CHECK(call->base.kind == NodeCall && strcmp(call->callee, "pass") == 0);
    STRATA_CHECK_EQ((long)call->typeArgCount, 1);
    STRATA_CHECK(call->typeArg == &call->typeArgs[0]);
    STRATA_CHECK(NameIs(&call->typeArgs[0], "int"));
    STRATA_CHECK(IdentIs(VecGet(&call->args, 0), "x"));

    VarDeclStmt* zeros = (VarDeclStmt*)CloneBodyStmt(fn, 2);
    STRATA_CHECK(NameIs(&zeros->type, "int[4]"));
    ArrayInitExpr* arrayInit = (ArrayInitExpr*)zeros->init;
    STRATA_CHECK(NameIs(arrayInit->elementType, "int"));
    STRATA_CHECK_EQ((long)arrayInit->elements.count, 1);
    STRATA_CHECK(arrayInit->repeatCount != NULL);

    MemberExpr* member = (MemberExpr*)((ReturnStmt*)CloneBodyStmt(fn, 3))->value;
    STRATA_CHECK(member->base.kind == NodeMember && strcmp(member->member, "max") == 0);
    STRATA_CHECK(IdentIs(member->baseNode, "int"));

    // The template keeps its own names, and the clone owns fresh lists.
    STRATA_CHECK(NameIs(&tpl->returnType, "T"));
    STRATA_CHECK(NameIs(&ParamOf(tpl, 0)->type, "T"));
    STRATA_CHECK(ParamOf(tpl, 0)->type.isConst);
    STRATA_CHECK(NameIs(&ParamOf(tpl, 1)->type, "^T"));
    STRATA_CHECK(NameIs(&ParamOf(tpl, 2)->type, "T[]"));
    STRATA_CHECK(NameIs(&ParamOf(tpl, 4)->type, "number<T>"));
    STRATA_CHECK(NameIs(&ParamOf(tpl, 7)->type, "T[2][6]"));
    STRATA_CHECK(NameIs(&((VarDeclStmt*)CloneBodyStmt(tpl, 0))->type, "number<T>"));
    STRATA_CHECK(IdentIs(((MemberExpr*)((ReturnStmt*)CloneBodyStmt(tpl, 3))->value)->baseNode, "T"));
    STRATA_CHECK(fn->params.items != tpl->params.items);
    STRATA_CHECK(((Block*)fn->body)->statements.items != ((Block*)tpl->body)->statements.items);

    AstDispose((Node*)fn);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_clone_substitutes_box_argument)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(kCloneSource, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* tpl = (FunctionDecl*)VecGet(&mod->functions, 0);
    TypeName args[1] = {TypeNameParse(&arena, "^Foo")};
    TypeSubst s = {&tpl->genericParams, args};
    FunctionDecl* fn = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, &s);

    STRATA_CHECK(NameIs(&fn->returnType, "^Foo"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 0)->type, "^Foo"));
    STRATA_CHECK(ParamOf(fn, 0)->type.isConst);
    STRATA_CHECK(NameIs(&ParamOf(fn, 1)->type, "^^Foo"));
    STRATA_CHECK(ParamOf(fn, 1)->type.isBox && NameIs(ParamOf(fn, 1)->type.inner, "^Foo"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 2)->type, "^Foo[]"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 3)->type, "^Foo[4]"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 4)->type, "number<^Foo>"));
    STRATA_CHECK(NameIs(TypeArg(&ParamOf(fn, 4)->type, 0), "^Foo"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 5)->type, "^Foo?"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 6)->type, "*^Foo"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 7)->type, "^Foo[2][6]"));

    VarDeclStmt* local = (VarDeclStmt*)CloneBodyStmt(fn, 0);
    STRATA_CHECK(NameIs(&local->type, "number<^Foo>"));
    STRATA_CHECK(strcmp(((StructInitExpr*)local->init)->typeName, "number<^Foo>") == 0);

    CallExpr* call = (CallExpr*)((VarDeclStmt*)CloneBodyStmt(fn, 1))->init;
    STRATA_CHECK(NameIs(&call->typeArgs[0], "^Foo"));
    STRATA_CHECK(NameIs(((ArrayInitExpr*)((VarDeclStmt*)CloneBodyStmt(fn, 2))->init)->elementType, "^Foo"));
    STRATA_CHECK(IdentIs(((MemberExpr*)((ReturnStmt*)CloneBodyStmt(fn, 3))->value)->baseNode, "^Foo"));

    // The template still names T.
    STRATA_CHECK(NameIs(&ParamOf(tpl, 1)->type, "^T"));

    AstDispose((Node*)fn);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_clone_substitutes_generic_use_argument)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* argMod = ParseModule("int f() { number<int> x; return 0; }", &diag, &arena);
    Module* mod = ParseModule(kCloneSource, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // The argument is the generic use `number<int>` as the parser wrote it.
    FunctionDecl* tpl = (FunctionDecl*)VecGet(&mod->functions, 0);
    TypeName args[1] = {*DeclTypeOf(argMod, 0)};
    TypeSubst s = {&tpl->genericParams, args};
    FunctionDecl* fn = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, &s);

    STRATA_CHECK(NameIs(&fn->returnType, "number<int>"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 0)->type, "number<int>"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 1)->type, "^number<int>"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 2)->type, "number<int>[]"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 3)->type, "number<int>[4]"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 5)->type, "number<int>?"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 6)->type, "*number<int>"));
    STRATA_CHECK(NameIs(&ParamOf(fn, 7)->type, "number<int>[2][6]"));

    // `number<T>` with T = number<int>: the argument is nested, and the spelling is rebuilt.
    const TypeName* h = &ParamOf(fn, 4)->type;
    STRATA_CHECK(NameIs(h, "number<number<int>>"));
    STRATA_CHECK(strcmp(h->genericBase, "number") == 0);
    STRATA_CHECK(NameIs(TypeArg(h, 0), "number<int>"));
    STRATA_CHECK(strcmp(TypeArg(h, 0)->genericBase, "number") == 0);
    STRATA_CHECK(NameIs(TypeArg(TypeArg(h, 0), 0), "int"));

    VarDeclStmt* local = (VarDeclStmt*)CloneBodyStmt(fn, 0);
    STRATA_CHECK(NameIs(&local->type, "number<number<int>>"));
    STRATA_CHECK(strcmp(((StructInitExpr*)local->init)->typeName, "number<number<int>>") == 0);

    CallExpr* call = (CallExpr*)((VarDeclStmt*)CloneBodyStmt(fn, 1))->init;
    STRATA_CHECK(NameIs(&call->typeArgs[0], "number<int>"));

    AstDispose((Node*)fn);
    AstDispose((Node*)mod);
    AstDispose((Node*)argMod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_clone_struct_defaults_and_impl)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct number<T> { T value; int count = 3; }\n"
                              "impl number<T> {\n"
                              "    property T size { get = number_get_size; set = number_set_size; }\n"
                              "    T get(const ref number<T> self) { return self.value; }\n"
                              "}",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    StructDecl* tpl = (StructDecl*)VecGet(&mod->structs, 0);
    ImplDecl* implTpl = (ImplDecl*)VecGet(&mod->impls, 0);
    TypeName args[1] = {TypeNameLeaf("int")};
    TypeSubst s = {&tpl->genericParams, args};

    StructDecl* sd = (StructDecl*)AstCloneSubst(&arena, (Node*)tpl, &s);
    STRATA_CHECK_EQ((long)sd->fields.count, 2);
    FieldDecl* value = (FieldDecl*)VecGet(&sd->fields, 0);
    FieldDecl* count = (FieldDecl*)VecGet(&sd->fields, 1);
    STRATA_CHECK(strcmp(value->name, "value") == 0);
    STRATA_CHECK(NameIs(&value->type, "int"));
    STRATA_CHECK(NameIs(&count->type, "int"));
    STRATA_CHECK(count->defaultValue != NULL && count->defaultValue->kind == NodeIntLiteral);
    STRATA_CHECK(((IntLiteral*)count->defaultValue)->value == 3);
    STRATA_CHECK(count->defaultValue != ((FieldDecl*)VecGet(&tpl->fields, 1))->defaultValue);
    STRATA_CHECK_EQ((long)sd->genericParams.count, 1);
    STRATA_CHECK(sd->genericParams.items != tpl->genericParams.items);
    STRATA_CHECK(sd->dropFunction == NULL);

    // The template keeps `T` and its own default.
    STRATA_CHECK(NameIs(&((FieldDecl*)VecGet(&tpl->fields, 0))->type, "T"));

    ImplDecl* impl = (ImplDecl*)AstCloneSubst(&arena, (Node*)implTpl, &s);
    STRATA_CHECK(strcmp(impl->handleName, "number") == 0);
    STRATA_CHECK_EQ((long)impl->methods.count, 1);
    FunctionDecl* method = (FunctionDecl*)VecGet(&impl->methods, 0);
    STRATA_CHECK(strcmp(method->name, "number_get") == 0);
    STRATA_CHECK(NameIs(&method->returnType, "int"));
    STRATA_CHECK(NameIs(&ParamOf(method, 0)->type, "number<int>"));
    STRATA_CHECK(IdentIs(((MemberExpr*)((ReturnStmt*)CloneBodyStmt(method, 0))->value)->baseNode, "self"));

    STRATA_CHECK_EQ((long)impl->properties.count, 1);
    PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, 0);
    STRATA_CHECK(strcmp(prop->name, "size") == 0);
    STRATA_CHECK(NameIs(&prop->returnType, "int"));
    STRATA_CHECK(strcmp(prop->getterSymbol, "number_get_size") == 0);
    STRATA_CHECK(strcmp(prop->setterSymbol, "number_set_size") == 0);
    STRATA_CHECK(NameIs(&((PropertyDecl*)VecGet(&implTpl->properties, 0))->returnType, "T"));

    // The method is listed in the module's functions in a module; here the clone owns it.
    AstDispose((Node*)method);
    AstDispose((Node*)impl);
    AstDispose((Node*)sd);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_clone_without_substitution_is_deep_copy)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int id<T>(T x) { T y = x; return 0; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* tpl = (FunctionDecl*)VecGet(&mod->functions, 0);

    // A NULL substitution, and one with no parameters, both copy the names unchanged.
    Vec empty;
    VecInit(&empty);
    TypeSubst none = {&empty, NULL};

    FunctionDecl* plain = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, NULL);
    FunctionDecl* noParams = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, &none);

    STRATA_CHECK(NameIs(&plain->returnType, "int"));
    STRATA_CHECK(NameIs(&ParamOf(plain, 0)->type, "T"));
    STRATA_CHECK(NameIs(&ParamOf(noParams, 0)->type, "T"));
    STRATA_CHECK(plain->body != tpl->body);
    STRATA_CHECK(IdentIs(((VarDeclStmt*)CloneBodyStmt(plain, 0))->init, "x"));
    STRATA_CHECK(NameIs(&((VarDeclStmt*)CloneBodyStmt(noParams, 0))->type, "T"));

    AstDispose((Node*)plain);
    AstDispose((Node*)noParams);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_clone_covers_every_node_kind)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("import lib;\n"
                              "handle Window;\n"
                              "enum Mode { Fast, Slow = 2 }\n"
                              "int limit = 3;\n"
                              "int[] values = { 1, 2 };\n"
                              "struct Foo { int a; }\n"
                              "struct number<T> { T value; int count = 3; }\n"
                              "impl number<T> {\n"
                              "    property T size { get = number_get_size; set = number_set_size; }\n"
                              "    T get(const ref number<T> self) { return self.value; }\n"
                              "}\n"
                              "float scale<T>(const T x, T[] items, T[4] slots, number<T> h, T? maybe, *T ptr)\n"
                              "{\n"
                              "    number<T> local = number<T> { .value = x };\n"
                              "    T copy = pass<T>(x);\n"
                              "    T[4] zeros = { x; 4 };\n"
                              "    int[2] pair = { 1, 2 };\n"
                              "    string str = \"hi\";\n"
                              "    float f = 1.5;\n"
                              "    bool ok = true;\n"
                              "    int c = 0;\n"
                              "    int d = -c + 1 * 2;\n"
                              "    int pv = *ptr;\n"
                              "    *int q = &c;\n"
                              "    c += 1;\n"
                              "    c = c - 1;\n"
                              "    c++;\n"
                              "    ++c;\n"
                              "    float fl = (float)c;\n"
                              "    bool nt = maybe?;\n"
                              "    bool nn = !ok;\n"
                              "    int first = pair[0];\n"
                              "    int tern = c > 0 ? 1 : 2;\n"
                              "    Foo foo = Foo { .a = 1 };\n"
                              "    local.get();\n"
                              "    g(1, 2);\n"
                              "    if (c < 4) { c = 1; } else { c = 2; }\n"
                              "    while (c > 0) { c--; if (c == 2) { break; } continue; }\n"
                              "    for (int i = 0; i < 3; i++) { c = c + i; }\n"
                              "    defer c = 0;\n"
                              "    return 1.0;\n"
                              "}",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // `scale` is last: the impl's methods are listed in Module::functions before it.
    FunctionDecl* tpl = (FunctionDecl*)VecGet(&mod->functions, mod->functions.count - 1);
    STRATA_CHECK(strcmp(tpl->name, "scale") == 0);

    TypeName args[1] = {TypeNameLeaf("int")};
    TypeSubst s = {NULL, NULL};
    TypeSubst t = {&tpl->genericParams, args};

    // Every top-level declaration, cloned with and without a substitution.
    for (size_t i = 0; i < mod->imports.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->imports, i), &t);
    }

    for (size_t i = 0; i < mod->handles.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->handles, i), &t);
    }

    for (size_t i = 0; i < mod->enums.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->enums, i), &t);
    }

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->globals, i), &t);
        CloneAndDispose(&arena, (Node*)VecGet(&mod->globals, i), &s);
    }

    for (size_t i = 0; i < mod->structs.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->structs, i), &t);
    }

    for (size_t i = 0; i < mod->impls.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->impls, i), &t);
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        CloneAndDispose(&arena, (Node*)VecGet(&mod->functions, i), &t);
        CloneAndDispose(&arena, (Node*)VecGet(&mod->functions, i), &s);
    }

    // The body clone keeps the template's statement kinds, in order.
    FunctionDecl* fn = (FunctionDecl*)AstCloneSubst(&arena, (Node*)tpl, &t);
    STRATA_CHECK_EQ((long)((Block*)fn->body)->statements.count, (long)((Block*)tpl->body)->statements.count);

    for (size_t i = 0; i < ((Block*)tpl->body)->statements.count; i++)
    {
        STRATA_CHECK(CloneBodyStmt(fn, i)->kind == CloneBodyStmt(tpl, i)->kind);
    }

    AstDispose((Node*)fn);
    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

// -- Monomorphize (phase 3)

static const StructDecl* StructNamed(const Module* m, const char* name)
{
    for (size_t i = 0; i < m->structs.count; i++)
    {
        const StructDecl* sd = (const StructDecl*)VecGet(&m->structs, i);

        if (strcmp(sd->name, name) == 0)
        {
            return sd;
        }
    }

    return NULL;
}

static const FunctionDecl* FunctionNamed(const Module* m, const char* name)
{
    for (size_t i = 0; i < m->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&m->functions, i);

        if (strcmp(fn->name, name) == 0)
        {
            return fn;
        }
    }

    return NULL;
}

STRATA_TEST(monomorph_struct_use_becomes_instance_name)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "int main() { number<int> a = number<int> { .value = 3 }; return a.value; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK(StructNamed(mod, "number") == NULL);
    STRATA_CHECK(StructNamed(mod, "number<int>") != NULL);
    STRATA_CHECK_EQ((long)mod->genericTemplates.count, 1);

    // The local's type and the literal's type are the plain instance name now.
    const TypeName* type = DeclTypeOf(mod, 0);
    STRATA_CHECK(strcmp(type->name, "number<int>") == 0);
    STRATA_CHECK_EQ((long)type->typeArgCount, 0);
    STRATA_CHECK(type->genericBase == NULL);

    StructInitExpr* init = (StructInitExpr*)InitOf(mod, 0);
    STRATA_CHECK(strcmp(init->typeName, "number<int>") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_impl_methods_are_named_per_instance)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "impl number<T> { T get_value(const ref number<T> self) { return self.value; } }\n"
                                  "int main()\n"
                                  "{\n"
                                  "    number<int> a = number<int> { .value = 1 };\n"
                                  "    number<float> b = number<float> { .value = 1.5 };\n"
                                  "    return a.get_value();\n"
                                  "}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // One impl per instance, each with its own method symbol; the template and its method are kept aside.
    STRATA_CHECK_EQ((long)mod->impls.count, 2);
    STRATA_CHECK_EQ((long)mod->genericTemplates.count, 3);

    const ImplDecl* impl = ModuleFindImpl(mod, "number<int>");
    STRATA_CHECK(impl != NULL);
    STRATA_CHECK_EQ((long)impl->genericParams.count, 0);

    const FunctionDecl* method = FunctionNamed(mod, "number<int>_get_value");
    STRATA_CHECK(method != NULL);
    STRATA_CHECK(strcmp(method->methodName, "get_value") == 0);
    STRATA_CHECK(strcmp(method->mangledName, "number<int>_get_value") == 0);
    STRATA_CHECK(method->fromImpl);
    STRATA_CHECK(FunctionNamed(mod, "number<float>_get_value") != NULL);
    STRATA_CHECK(FunctionNamed(mod, "number_get_value") == NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_generic_call_names_its_instance)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "number<T> make_number<T>(T v) { number<T> n = { .value = v }; return n; }\n"
                                  "int main() { number<int> a = make_number<int>(3); return a.value; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK(FunctionNamed(mod, "make_number") == NULL);
    const FunctionDecl* instance = FunctionNamed(mod, "make_number<int>");
    STRATA_CHECK(instance != NULL);
    STRATA_CHECK(strcmp(instance->mangledName, "make_number<int>") == 0);
    STRATA_CHECK_EQ((long)instance->genericParams.count, 0);

    // The call now names the instance, with no explicit type arguments left to resolve.
    CallExpr* call = (CallExpr*)InitOf(mod, 0);
    STRATA_CHECK(strcmp(call->callee, "make_number<int>") == 0);
    STRATA_CHECK_EQ((long)call->typeArgCount, 0);
    STRATA_CHECK(call->typeArg == NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_nested_instances_get_their_own_structs)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "int main()\n"
                                  "{\n"
                                  "    number<number<number<int>>> n;\n"
                                  "    return 0;\n"
                                  "}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK(StructNamed(mod, "number<int>") != NULL);
    STRATA_CHECK(StructNamed(mod, "number<number<int>>") != NULL);
    STRATA_CHECK(StructNamed(mod, "number<number<number<int>>>") != NULL);
    STRATA_CHECK(strcmp(DeclTypeOf(mod, 0)->name, "number<number<number<int>>>") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_return_literal_of_generic_type_is_substituted)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "number<T> make<T>(T v) { return { .value = v }; }\n"
                                  "int main() { number<float> f = make<float>(2.0); return 0; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const FunctionDecl* made = FunctionNamed(mod, "make<float>");
    STRATA_CHECK(made != NULL);
    ReturnStmt* ret = (ReturnStmt*)VecGet(&((Block*)made->body)->statements, 0);
    STRATA_CHECK_EQ((long)ret->value->kind, (long)NodeStructInit);
    STRATA_CHECK(strcmp(((StructInitExpr*)ret->value)->typeName, "number<float>") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_parser_keeps_generic_literal_type)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct number<T> { T value; }\n"
                              "number<T> make<T>(T v) { return { .value = v }; }\n"
                              "void keep<T>(T v) { number<T> n = { .value = v }; }\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // A typed `{ ... }` literal records the full generic type, so cloning a generic body can substitute it.
    const FunctionDecl* make = (const FunctionDecl*)VecGet(&mod->functions, 0);
    StructInitExpr* ret = (StructInitExpr*)((ReturnStmt*)VecGet(&((Block*)make->body)->statements, 0))->value;
    STRATA_CHECK(ret->genericType != NULL);
    STRATA_CHECK(strcmp(ret->genericType->genericBase, "number") == 0);

    const FunctionDecl* keep = (const FunctionDecl*)VecGet(&mod->functions, 1);
    VarDeclStmt* decl = (VarDeclStmt*)VecGet(&((Block*)keep->body)->statements, 0);
    STRATA_CHECK(((StructInitExpr*)decl->init)->genericType != NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_reports_misuse_of_templates)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"struct number<T> { T value; }\nint main() { number x; return 0; }",
         "generic struct 'number' needs type arguments"                                                                                        },
        {"struct number<T> { T value; }\nint main() { number<int, int> x; return 0; }",
         "generic struct 'number' takes 1 type argument(s), but 2 were given"                                                                  },
        {"struct plain { int a; }\nint main() { plain<int> x; return 0; }",                                   "'plain' is not a generic struct"},
        {"struct number<T> { T value; }\nint main() { number<int> x = number { .value = 1 }; return 0; }",
         "generic struct 'number' needs type arguments"                                                                                        },
        {"struct number<T> { T value; }\nint main() { number(3); return 0; }",
         "generic struct 'number' needs type arguments"                                                                                        },
        {"int make<T>(T v) { return 0; }\nint main() { return make(3); }",
         "generic function 'make' needs explicit type arguments: make<...>(...)"                                                               },
        {"int make<T>(T v) { return 0; }\nint main() { return make<int, int>(3); }",
         "generic function 'make' takes 1 type argument(s), but 2 were given"                                                                  },
        {"int pick<T>(T v) { return 0; }\nint pick<T>(T v, T w) { return 1; }",
         "generic function 'pick' cannot be overloaded"                                                                                        },
        {"int pick<T>(T v) { return 0; }\nint pick(int v) { return 1; }",
         "generic function 'pick' cannot be overloaded"                                                                                        },
        {"struct number<T> { T value; }\nint number<T>(T v) { return 0; }",
         "function 'number' has the name of a struct"                                                                                          },
        {"struct number<T> { T value; }\nimpl number<A, B> { int get(const ref number self) { return 0; } }",
         "impl 'number' binds 2 type parameter(s), but struct 'number' has 1"                                                                  },
        {"struct number<T> { T value; }\nimpl number { int get(const ref number self) { return 0; } }",
         "generic struct 'number' needs an impl with its type parameters"                                                                      },
        {"struct plain { int a; }\nimpl plain<T> { int get(const ref plain self) { return 0; } }",
         "'plain' is not a generic struct"                                                                                                     },
        {"struct number<T> { T value; }\nimpl number<T> { property int P { get = number_get_p; } }",
         "property 'P' in a generic impl is not supported"                                                                                     },
        {"struct list<T> { list<list<T>>? next; }\nint main() { list<int> l; return 0; }",
         "generic instantiation of 'list' is too deep"                                                                                         },
        {"struct flip<A, B> { flip<B, flip<A, B>>? next; }\nint main() { flip<int, float> f; return 0; }",
         "generic instantiation of 'flip' is too deep"                                                                                         },
        {"T plus1<T>(T v) { return v + 1; }\nint main() { string s = plus1<string>(\"x\"); return 0; }",
         "invalid operands to binary operator"                                                                                                 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Arena arena;
        ArenaInit(&arena, 0);
        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);
        Module* mod = ParseAndResolve(cases[i].src, &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        STRATA_CHECK(DiagsContaining(&diag, cases[i].needle) >= 1);
        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        ArenaFree(&arena);
    }
}

STRATA_TEST(monomorph_leaves_plain_modules_alone)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule(
        "struct point { int x; int y; }\nint main() { point p = point { .x = 1 }; return p.x; }\n", &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->structs.count, 1);
    STRATA_CHECK_EQ((long)mod->functions.count, 1);
    STRATA_CHECK_EQ((long)mod->genericTemplates.count, 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_direct_call_moves_templates_out)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod
        = ParseModule("struct pair<A, B> { A first; B second; }\n"
                      "impl pair<A, B> { A get_first(const ref pair<A, B> self) { return self.first; } }\n"
                      "int main() { pair<int, float> p = pair<int, float> { .first = 1, .second = 2.0 }; return 0; }\n",
                      &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK(StructNamed(mod, "pair") == NULL);
    STRATA_CHECK(StructNamed(mod, "pair<int,float>") != NULL);
    STRATA_CHECK(ModuleFindImpl(mod, "pair<int,float>") != NULL);
    STRATA_CHECK(FunctionNamed(mod, "pair<int,float>_get_first") != NULL);
    // Two impl methods and the impl, moved out of the module lists: the struct, the impl and the method.
    STRATA_CHECK_EQ((long)mod->genericTemplates.count, 3);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_bracketed_type_argument_is_one_name)
{
    Arena arena;
    ArenaInit(&arena, 0);

    // `[` inside a type-argument list belongs to the argument: the whole spelling is one leaf.
    TypeName inner = TypeNameParse(&arena, "number<int[]>");
    STRATA_CHECK(!TypeNameIsArray(&inner));
    STRATA_CHECK(strcmp(inner.name, "number<int[]>") == 0);

    // A bracket after the closing `>` is still the array dimension.
    TypeName dims = TypeNameParse(&arena, "number<int>[2]");
    STRATA_CHECK(TypeNameIsFixedArray(&dims));
    STRATA_CHECK(strcmp(TypeNameArrayElem(&dims)->name, "number<int>") == 0);

    ArenaFree(&arena);
}

STRATA_TEST(monomorph_generic_array_argument_runs_through_sema)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("struct number<T> { T value; }\n"
                                  "int main()\n"
                                  "{\n"
                                  "    ^number<int[]> arr = { .value = { 1, 2, 3 } };\n"
                                  "    return arr.value[2];\n"
                                  "}\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(StructNamed(mod, "number<int[]>") != NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_pack_declares_last_type_parameter_as_pack)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void f<A, T...>(A a, T... rest) {}\nvoid g<T>(T x) {}", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(f->genericPack);
    STRATA_CHECK_EQ((long)f->genericParams.count, 2);
    STRATA_CHECK(strcmp((char*)VecGet(&f->genericParams, 1), "T") == 0);
    const ParamDecl* rest = ParamOf(f, 1);
    STRATA_CHECK(rest->isVarargRest);
    STRATA_CHECK(TypeNameIsArray(&rest->type));
    STRATA_CHECK(strcmp(TypeNameArrayElem(&rest->type)->name, "T") == 0);
    STRATA_CHECK(strcmp(rest->name, "rest") == 0);

    FunctionDecl* g = (FunctionDecl*)VecGet(&mod->functions, 1);
    STRATA_CHECK(!g->genericPack);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_pack_rejected_declarations_report_errors)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"void f<T..., U>(T... a) {}",               "a parameter pack must be the last type parameter"       },
        {"struct s<T...> { int x; }",                "parameter packs are only supported on generic functions"},
        {"struct s<T> { int x; }\nimpl s<T...> { }", "parameter packs are only supported on generic functions"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Arena arena;
        ArenaInit(&arena, 0);
        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);
        Module* mod = ParseModule(cases[i].src, &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        STRATA_CHECK(DiagsContaining(&diag, cases[i].needle) >= 1);
        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        ArenaFree(&arena);
    }
}

STRATA_TEST(generic_pack_expansions_parse_in_type_and_call_positions)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void g<A, B>(A a, B b) {}\n"
                              "void f<T...>(T... args)\n"
                              "{\n"
                              "    g<int, T...>(1, args...);\n"
                              "    x.m(args...);\n"
                              "    h<T...>();\n"
                              "}\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, 1);
    CallExpr* g = (CallExpr*)((ExprStmt*)CloneBodyStmt(f, 0))->expr;
    STRATA_CHECK(g->base.kind == NodeCall);
    STRATA_CHECK(g->hasTypeArgList);
    STRATA_CHECK_EQ((long)g->typeArgCount, 2);
    STRATA_CHECK(!g->typeArgs[0].isPackExpansion);
    STRATA_CHECK(g->typeArgs[1].isPackExpansion);
    STRATA_CHECK(strcmp(g->typeArgs[1].name, "T") == 0);
    STRATA_CHECK_EQ((long)g->args.count, 2);
    STRATA_CHECK(((Node*)VecGet(&g->args, 1))->kind == NodePackExpand);
    STRATA_CHECK(strcmp(((PackExpandExpr*)VecGet(&g->args, 1))->name, "args") == 0);

    CallExpr* m = (CallExpr*)((ExprStmt*)CloneBodyStmt(f, 1))->expr;
    STRATA_CHECK(m->calleeBase != NULL);
    STRATA_CHECK(((Node*)VecGet(&m->args, 0))->kind == NodePackExpand);

    // `h<T...>()` with an empty pack is not a parse error, and the list is recorded.
    CallExpr* h = (CallExpr*)((ExprStmt*)CloneBodyStmt(f, 2))->expr;
    STRATA_CHECK(h->hasTypeArgList);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(generic_pack_empty_list_and_comparisons_parse)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod
        = ParseModule("extern int printf(cstring fmt, ...);\n"
                      "int sum(int... rest) { return 0; }\n"
                      "int main() { int a = 1; int b = 2; bool c = a < b; f<>(); f(); return a < b ? 1 : 0; }\n",
                      &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* main = (FunctionDecl*)VecGet(&mod->functions, 2);
    CallExpr* empty = (CallExpr*)((ExprStmt*)CloneBodyStmt(main, 3))->expr;
    STRATA_CHECK(empty->hasTypeArgList);
    STRATA_CHECK_EQ((long)empty->typeArgCount, 0);

    CallExpr* plain = (CallExpr*)((ExprStmt*)CloneBodyStmt(main, 4))->expr;
    STRATA_CHECK(!plain->hasTypeArgList);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(monomorph_reports_pack_misuse)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"void f<T...>(T... a) { int x = a; }\nint main() { f<int>(1); return 0; }",
         "parameter pack 'a' can only be used as"                                                                                                                      },
        {"void f<T...>(T... a) { T x; }\nint main() { f<int>(1); return 0; }",
         "parameter pack 'T' must be expanded: write 'T...'"                                                                                                           },
        {"void f<T...>(T... a) { int i = 0; int x = a[i]; }\nint main() { f<int>(1); return 0; }",
         "a parameter pack can only be indexed by an integer literal"                                                                                                  },
        {"void f<T...>(T... a) { for (int i = 0; i < 1; i++) { } for (int j = 0; j < a.length; i++) { } }\n"
         "int main() { f<int>(1); return 0; }",                                           "a 'for' loop over a parameter pack must be written"                },
        {"void f<T...>(T... a) { for (int i = 0; i < a.length; i++) { break; } }\n"
         "int main() { f<int>(1); return 0; }",                                           "'break' is not supported inside a 'for' loop over a parameter pack"},
        {"void g<T>(T x) {}\nvoid h(int a) { g<int...>(a); }\nint main() { return 0; }",
         "'int...' expands a parameter pack, but 'int' is not one"                                                                                                     },
        {"void f<A, B...>(A a, B... b) {}\nint main() { f<>(1); return 0; }",
         "generic function 'f' takes at least 1 type argument(s), but 0 were given"                                                                                    },
        {"void f<T...>(T... a) {}\nint main() { f(1); return 0; }",
         "generic function 'f' needs explicit type arguments"                                                                                                          },
        {"int x = 1;\nvoid h(int a) { x(a...); }\nint main() { return 0; }",
         "'a...' expands a parameter pack, but 'a' is not one"                                                                                                         },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        Arena arena;
        ArenaInit(&arena, 0);
        DiagnosticEngine diag;
        DiagnosticEngineInit(&diag);
        Module* mod = ParseAndResolve(cases[i].src, &diag, &arena);
        STRATA_CHECK(DiagHasErrors(&diag));
        STRATA_CHECK(DiagsContaining(&diag, cases[i].needle) >= 1);
        AstDispose((Node*)mod);
        DiagnosticEngineFree(&diag);
        ArenaFree(&arena);
    }
}

STRATA_TEST(monomorph_pack_expands_into_instance_parameters)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("void show(int v) {}\n"
                                  "void each<T...>(T... args)\n"
                                  "{\n"
                                  "    for (int i = 0; i < args.length; i++) { show(args[i]); }\n"
                                  "}\n"
                                  "int main() { each<int, int>(1, 2); return 0; }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* inst = NULL;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(fn->name, "each<int,int>") == 0)
        {
            inst = fn;
        }
    }

    STRATA_CHECK(inst != NULL);
    STRATA_CHECK(!inst->genericPack);
    STRATA_CHECK(!inst->isVariadic);
    STRATA_CHECK_EQ((long)inst->params.count, 2);
    STRATA_CHECK(strcmp(ParamOf(inst, 0)->name, "args[0]") == 0);
    STRATA_CHECK(strcmp(ParamOf(inst, 1)->name, "args[1]") == 0);
    STRATA_CHECK(strcmp(ParamOf(inst, 1)->type.name, "int") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}
