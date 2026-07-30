#include "Util.h"
#include "strata/Test.h"

static ReturnStmt* SingleReturn(Module* m)
{
    STRATA_CHECK(m != NULL);
    if (m->functions.count == 0)
    {
        return NULL;
    }

    FunctionDecl* fn = (FunctionDecl*)VecGet(&m->functions, 0);
    if (!fn->body)
    {
        return NULL;
    }

    Block* block = (Block*)fn->body;
    if (block->statements.count == 0)
    {
        return NULL;
    }

    return (ReturnStmt*)VecGet(&block->statements, 0);
}

STRATA_TEST(parser_simple_function)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return 7; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));
    STRATA_CHECK(mod->functions.count == 1);

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK(strcmp(fn->name, "f") == 0);
    STRATA_CHECK(strcmp(fn->returnType.name, "int") == 0);

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret != NULL);
    STRATA_CHECK(ret->value->kind == NodeIntLiteral);
    STRATA_CHECK_EQ((long)((IntLiteral*)ret->value)->value, 7);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_out_parameter)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void g(out int x) {}", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    STRATA_CHECK_EQ((long)fn->params.count, 1);

    ParamDecl* p = (ParamDecl*)VecGet(&fn->params, 0);
    STRATA_CHECK(p->mod == ModOut);
    STRATA_CHECK(strcmp(p->type.name, "int") == 0);
    STRATA_CHECK(strcmp(p->name, "x") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_inout_parameter)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("void h(inout float t) {}", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    ParamDecl* p = (ParamDecl*)VecGet(&fn->params, 0);
    STRATA_CHECK(p->mod == ModInOut);
    STRATA_CHECK(strcmp(p->type.name, "float") == 0);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_binary_precedence)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return 1 + 2 * 3; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret->value->kind == NodeBinary);

    BinaryExpr* add = (BinaryExpr*)ret->value;
    STRATA_CHECK(add->op == BinAdd);
    STRATA_CHECK(add->lhs->kind == NodeIntLiteral);
    STRATA_CHECK(add->rhs->kind == NodeBinary);

    BinaryExpr* mul = (BinaryExpr*)add->rhs;
    STRATA_CHECK(mul->op == BinMul);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_var_decl)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { int x = 5; return x; }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    Block* block = (Block*)fn->body;
    STRATA_CHECK(((Node*)VecGet(&block->statements, 0))->kind == NodeVarDecl);

    VarDeclStmt* vd = (VarDeclStmt*)VecGet(&block->statements, 0);
    STRATA_CHECK(strcmp(vd->name, "x") == 0);
    STRATA_CHECK(vd->init != NULL);
    STRATA_CHECK(vd->init->kind == NodeIntLiteral);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_call_and_member)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f() { return add(1, 2); }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    ReturnStmt* ret = SingleReturn(mod);
    STRATA_CHECK(ret->value->kind == NodeCall);

    CallExpr* call = (CallExpr*)ret->value;
    STRATA_CHECK(strcmp(call->callee, "add") == 0);
    STRATA_CHECK_EQ((long)call->args.count, 2);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_recovers_from_error)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f( { }", &diag, &arena);
    STRATA_CHECK(DiagHasErrors(&diag));
    STRATA_CHECK(DiagErrorCount(&diag) >= 1);
    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}

STRATA_TEST(parser_if_else)
{
    Arena arena; arena_init(&arena, 0);
    DiagnosticEngine diag; DiagnosticEngineInit(&diag);
    Module* mod = ParseModule("int f(int a) { if (a) { return 1; } else { return 2; } }", &diag, &arena);
    STRATA_CHECK(!DiagHasErrors(&diag));

    FunctionDecl* fn = (FunctionDecl*)VecGet(&mod->functions, 0);
    Block* block = (Block*)fn->body;
    STRATA_CHECK(((Node*)VecGet(&block->statements, 0))->kind == NodeIf);

    IfStmt* ifn = (IfStmt*)VecGet(&block->statements, 0);
    STRATA_CHECK(ifn->thenBranch != NULL);
    STRATA_CHECK(ifn->elseBranch != NULL);

    DiagnosticEngineFree(&diag);
    arena_free(&arena);
}
