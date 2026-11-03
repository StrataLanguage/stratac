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

// The value of `return` statement `i` of the first function.
static Node* ReturnValueOf(Module* m, size_t i)
{
    return ((ReturnStmt*)FirstBodyStmt(m, i))->value;
}

// The returned expression of the function `name`, after Monomorphize.
static const Node* ReturnOf(const Module* m, const char* name)
{
    for (size_t i = 0; i < m->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&m->functions, i);

        if (strcmp(fn->name, name) == 0)
        {
            return ((const ReturnStmt*)VecGet(&((const Block*)fn->body)->statements, 0))->value;
        }
    }

    return NULL;
}

static const MetaCallExpr* MetaCallOf(const Node* n)
{
    return (const MetaCallExpr*)n;
}

static const MetaArg* ArgOf(const MetaCallExpr* call, size_t i)
{
    return (const MetaArg*)VecGet(&call->args, i);
}

static const FunctionDecl* MetaNamed(const Module* m, const char* name)
{
    for (size_t i = 0; i < m->metaFunctions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&m->metaFunctions, i);

        if (strcmp(fn->name, name) == 0)
        {
            return fn;
        }
    }

    return NULL;
}

STRATA_TEST(meta_declaration_goes_to_meta_list)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@meta\n"
                              "bool droppable(type T)\n"
                              "{\n"
                              "    return @has_function(T, drop);\n"
                              "}\n"
                              "int plain() { return 1; }\n",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->metaFunctions.count, 1);
    STRATA_CHECK_EQ((long)mod->functions.count, 1);

    const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->metaFunctions, 0);
    STRATA_CHECK(fn->isMeta);
    STRATA_CHECK(strcmp(fn->name, "droppable") == 0);
    STRATA_CHECK(strcmp(((ParamDecl*)VecGet(&fn->params, 0))->type.name, "type") == 0);
    STRATA_CHECK(!((const FunctionDecl*)VecGet(&mod->functions, 0))->isMeta);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_call_splits_type_and_value_arguments)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return @pick(int, 3 + 1, flag, -2); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const MetaCallExpr* call = MetaCallOf(ReturnValueOf(mod, 0));
    STRATA_CHECK(call->base.kind == NodeMetaCall);
    STRATA_CHECK(strcmp(call->name, "pick") == 0);
    STRATA_CHECK_EQ((long)call->args.count, 4);

    // A type followed by `,` or `)` is a type argument. Anything else is read again as an expression, so `3 + 1`
    // and `-2` are expressions and `flag` (a bare name) is a type argument that the evaluator reads as a value.
    STRATA_CHECK(ArgOf(call, 0)->type != NULL && ArgOf(call, 0)->expr == NULL);
    STRATA_CHECK(strcmp(ArgOf(call, 0)->type->name, "int") == 0);
    STRATA_CHECK(ArgOf(call, 1)->expr != NULL && ArgOf(call, 1)->expr->kind == NodeBinary);
    STRATA_CHECK(ArgOf(call, 2)->type != NULL && strcmp(ArgOf(call, 2)->type->name, "flag") == 0);
    STRATA_CHECK(ArgOf(call, 3)->expr != NULL && ArgOf(call, 3)->expr->kind == NodeUnary);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_call_nested_type_argument)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("bool f() { return @is_same(number<int>, T[]); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    const MetaCallExpr* call = MetaCallOf(ReturnValueOf(mod, 0));
    const TypeName* left = ArgOf(call, 0)->type;
    const TypeName* right = ArgOf(call, 1)->type;
    STRATA_CHECK(strcmp(left->name, "number<int>") == 0);
    STRATA_CHECK(strcmp(left->genericBase, "number") == 0);
    STRATA_CHECK(TypeNameIsDynamicArray(right));
    STRATA_CHECK(strcmp(right->name, "T[]") == 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_hello_and_at_hello_coexist)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseAndResolve("int hello() { return 3; }\n"
                                  "@meta int hello(type T) { return 40; }\n"
                                  "int main() { return hello() + @hello(int); }\n",
                                  &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK_EQ((long)mod->metaFunctions.count, 1);
    STRATA_CHECK(MetaNamed(mod, "hello") != NULL);

    // The plain `hello` is the one in Module::functions; the meta call folded to 40 and did not find it.
    const Node* value = ReturnOf(mod, "main");
    STRATA_CHECK(value && value->kind == NodeBinary);
    STRATA_CHECK(((const BinaryExpr*)value)->rhs->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((const IntLiteral*)((const BinaryExpr*)value)->rhs)->value, 40);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_clone_substitutes_meta_call_type_argument)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void clear<T>(ref T x) { if (@droppable(T)) { x.drop(); } }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* tmpl = (FunctionDecl*)VecGet(&mod->functions, 0);
    Vec params;
    VecInit(&params);
    VecPush(&params, "T");
    TypeName arg = TypeNameLeaf("Widget");
    TypeSubst subst = {&params, &arg};

    FunctionDecl* copy = (FunctionDecl*)AstCloneSubst(&arena, (const Node*)tmpl, &subst);
    IfStmt* ifStmt = (IfStmt*)VecGet(&((Block*)copy->body)->statements, 0);
    const MetaCallExpr* call = MetaCallOf(ifStmt->condition);

    STRATA_CHECK(call->base.kind == NodeMetaCall);
    STRATA_CHECK(strcmp(call->name, "droppable") == 0);
    STRATA_CHECK(strcmp(ArgOf(call, 0)->type->name, "Widget") == 0);
    // The template keeps its own argument.
    const IfStmt* original = (const IfStmt*)VecGet(&((Block*)tmpl->body)->statements, 0);
    STRATA_CHECK(strcmp(ArgOf(MetaCallOf(original->condition), 0)->type->name, "T") == 0);

    free(params.items);
    AstDispose((Node*)mod);
    AstDispose((Node*)copy);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_dump_prints_calls_and_meta_functions)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("@meta bool same(type A, type B) { return @is_same(A, B); }\n"
                              "int main() { return 0; }",
                              &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    char* dump = DumpAst(mod, &arena);
    STRATA_CHECK(strstr(dump, "@meta\n") != NULL);
    STRATA_CHECK(strstr(dump, "(@is_same A B)") != NULL);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_parse_errors)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"int f() { return @(3); }",                                         "expected a meta function name after '@'"          },
        {"int f() { return @pick; }",                                        "expected '(' after '@pick'"                       },
        {"@meta extern int f(int n);",                                       "a @meta function cannot be extern: 'f'"           },
        {"@meta @export int f(int n) { return n; }",                         "a @meta function cannot be @export: 'f'"          },
        {"@meta int f<T>(int n) { return n; }",                              "a @meta function cannot have type parameters: 'f'"},
        {"struct A { int v; }\nimpl A { @meta int f(int n) { return n; } }",
         "a @meta function cannot be an impl method: 'f'"                                                                       },
        {"@meta int f(int n, ...) { return n; }",                            "a @meta function cannot be variadic: 'f'"         },
        {"@meta int f(int n);",                                              "a @meta function needs a body: 'f'"               },
        {"@meta struct S { int v; }",                                        "unknown struct attribute '@meta'"                 },
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

STRATA_TEST(meta_folds_builtin_results_to_literals)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("struct A { int v; }\n"
                              "impl A { void drop(ref A self) { self.v = 0; } }\n"
                              "int same() { return @is_same(int, int) ? 1 : 0; }\n"
                              "int has() { return @has_function(A, drop) ? 1 : 0; }\n"
                              "int not_same() { return @is_same(myint, int) ? 1 : 0; }\n",
                              &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // Each `?:` was pruned to the branch it took, so each return is the literal, with no conditional left.
    const Node* same = ReturnOf(mod, "same");
    STRATA_CHECK(same && same->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((const IntLiteral*)same)->value, 1);
    const Node* has = ReturnOf(mod, "has");
    STRATA_CHECK(has && has->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((const IntLiteral*)has)->value, 1);
    const Node* notSame = ReturnOf(mod, "not_same");
    STRATA_CHECK(notSame && notSame->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((const IntLiteral*)notSame)->value, 0);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_prunes_dead_if_before_rewriting_it)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    // The dropped branch calls a function that does not exist and names a missing generic struct. Neither is checked.
    Module* mod = ParseModule("@meta bool never() { return false; }\n"
                              "int f() { if (@never()) { int z = missing(1); box<Missing> b; } return 2; }",
                              &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    // The `if` became an empty block, which precedes the return.
    const Node* first = FirstBodyStmt(mod, 0);
    STRATA_CHECK(first->kind == NodeBlock);
    STRATA_CHECK_EQ((long)((const Block*)first)->statements.count, 0);
    STRATA_CHECK(FirstBodyStmt(mod, 1)->kind == NodeReturn);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_if_without_meta_call_is_not_pruned)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { if (false) { return 1; } return 2; }", &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(FirstBodyStmt(mod, 0)->kind == NodeIf);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}

STRATA_TEST(meta_monomorphize_reports_misuse)
{
    struct
    {
        const char* src;
        const char* needle;
    } cases[] = {
        {"int f() { return @nope(1); }",                                                          "unknown meta function '@nope'"                           },
        {"@meta int f(int a, int b) { return a + b; }\nint g() { return @f(1); }",
         "meta function '@f' takes 2 argument(s), but 1 were given"                                                                                         },
        {"@meta bool f(type T) { return true; }\nint g() { if (@f(3)) { return 1; } return 0; }",
         "meta function '@f' expects a type for parameter 'T'"                                                                                              },
        {"@meta int f(int n) { return n; }\nint g() { return @f(int); }",
         "meta function '@f' expects a value for parameter 'n', but 'int' is not a value"                                                                   },
        {"@meta int f() { while (true) { } return 1; }\nint g() { return @f(); }",
         "'while' can't be evaluated at compile time in meta function '@f'"                                                                                 },
        {"@meta int f(int n) { return @f(n + 1); }\nint g() { return @f(1); }",
         "meta function '@f' recurses too deeply"                                                                                                           },
        {"@meta int f(int n) { int x = n; }\nint g() { return @f(1); }",
         "meta function '@f' reaches its end without returning"                                                                                             },
        {"@meta int f() { return 1 / 0; }\nint g() { return @f(); }",                             "division by zero in a meta function"                     },
        {"@meta byte f() { return 300; }\nint g() { return @f(); }",                              "value 300 is out of range for 'byte'"                    },
        {"void f(type T) { }",                                                                    "'type' is only allowed for parameters of @meta functions"},
        {"void f<T>(type x) { }",                                                                 "'type' is only allowed for parameters of @meta functions"},
        {"struct box<T> { type v; }",                                                             "'type' is only allowed for parameters of @meta functions"},
        {"struct type { int v; }",                                                                "'type' is reserved and cannot name a struct"             },
        {"@meta bool has_function(type T) { return true; }",                                      "'@has_function' is a built-in meta function"             },
        {"@meta int f(float x) { return 1; }",
         "meta function '@f' parameter 'x' must be 'type', bool or an integer type"                                                                         },
        {"@meta float f() { return 1.0; }",                                                       "meta function '@f' must return bool or an integer type"  },
        {"@meta int f() { return 1; }\n@meta int f() { return 2; }",                              "redefinition of meta function '@f'"                      },
        {"int g() { return 1; }\n@meta int f(int n) { return g(); }",                             "a call can't be evaluated at compile time"               },
        {"@meta int f(int n) { for (;;) { } return n; }",                                         "'for' can't be evaluated at compile time"                },
        {"@meta int f() { int x; return 1; }",                                                    "local 'x' in meta function '@f' needs an initializer"    },
        {"@meta int f() { type T = int; return 1; }",                                             "'type' is only allowed for parameters of @meta functions"},
        {"@meta int f() { return 1; }\nint g() { int a = 4; return @f(a); }",
         "meta function '@f' takes 0 argument(s), but 1 were given"                                                                                         },
        {"@meta int f(int n) { return n; }\nint g() { int a = 4; return @f(a); }",
         "meta function '@f' expects a value for parameter 'n', but 'a' is not a value"                                                                     },
        {"@meta int f(ref int n) { return 1; }",                                                  "meta function '@f' parameter 'n' cannot be 'ref'"        },
        {"@meta int f() { return 1; }\nint g() { return @has_function(int); }",
         "'@has_function' takes 2 arguments, but 1 were given"                                                                                              },
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

STRATA_TEST(meta_has_function_sees_generic_template_impl)
{
    Arena arena;
    ArenaInit(&arena, 0);
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);

    // `number<int>` has no instance yet when the meta call is evaluated: its impl template defines `drop`. A wrapped
    // type has no impl of its own, and neither does a builtin.
    Module* mod = ParseModule("struct number<T> { T value; }\n"
                              "impl number<T> { void drop(ref number<T> self) { self.value = self.value; } }\n"
                              "int has() { return @has_function(number<int>, drop) ? 7 : 8; }\n"
                              "int missing() { return @has_function(number<int>, get) ? 7 : 8; }\n"
                              "int builtin() { return @has_function(int, drop) ? 7 : 8; }\n"
                              "int boxed() { return @has_function(^number<int>, drop) ? 7 : 8; }\n",
                              &diag, &arena);
    Monomorphize(mod, &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    STRATA_CHECK_EQ((long)((const IntLiteral*)ReturnOf(mod, "has"))->value, 7);
    STRATA_CHECK_EQ((long)((const IntLiteral*)ReturnOf(mod, "missing"))->value, 8);
    STRATA_CHECK_EQ((long)((const IntLiteral*)ReturnOf(mod, "builtin"))->value, 8);
    STRATA_CHECK_EQ((long)((const IntLiteral*)ReturnOf(mod, "boxed"))->value, 8);

    AstDispose((Node*)mod);
    DiagnosticEngineFree(&diag);
    ArenaFree(&arena);
}
