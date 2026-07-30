// Parser + AST unit tests.
#include "Util.hpp"
#include "strata/Test.hpp"

using namespace strata;
using namespace strata::test_util;

namespace
{
ReturnStmt* SingleReturn(Module* m)
{
    STRATA_CHECK(m != nullptr);
    if (m->functions.empty())
    {
        return nullptr;
    }

    auto& fn = m->functions.front();
    if (!fn->body)
    {
        return nullptr;
    }

    auto* block = static_cast<Block*>(fn->body.get());
    if (block->statements.empty())
    {
        return nullptr;
    }

    return static_cast<ReturnStmt*>(block->statements.front().get());
}
} // namespace

STRATA_TEST(parser_simple_function)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f() { return 7; }", diag);
    STRATA_CHECK(!diag.HasErrors());
    STRATA_CHECK(mod->functions.size() == 1);
    STRATA_CHECK(mod->functions[0]->name == "f");
    STRATA_CHECK(mod->functions[0]->returnType.name == "int");

    auto* ret = SingleReturn(mod.get());
    STRATA_CHECK(ret != nullptr);
    STRATA_CHECK(ret->value->kind == NodeKind::IntLiteral);
    STRATA_CHECK_EQ(static_cast<IntLiteral*>(ret->value.get())->value, (std::uint64_t)7);
}

STRATA_TEST(parser_out_parameter)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("void g(out int x) {}", diag);
    STRATA_CHECK(!diag.HasErrors());
    auto& fn = mod->functions[0];
    STRATA_CHECK_EQ(fn->params.size(), (std::size_t)1);
    STRATA_CHECK(fn->params[0]->mod == ParamMod::Out);
    STRATA_CHECK(fn->params[0]->type.name == "int");
    STRATA_CHECK(fn->params[0]->name == "x");
}

STRATA_TEST(parser_inout_parameter)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("void h(inout float t) {}", diag);
    STRATA_CHECK(!diag.HasErrors());
    STRATA_CHECK(mod->functions[0]->params[0]->mod == ParamMod::InOut);
    STRATA_CHECK(mod->functions[0]->params[0]->type.name == "float");
}

STRATA_TEST(parser_binary_precedence)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f() { return 1 + 2 * 3; }", diag);
    STRATA_CHECK(!diag.HasErrors());
    auto* ret = SingleReturn(mod.get());
    STRATA_CHECK(ret->value->kind == NodeKind::Binary);
    auto* add = static_cast<BinaryExpr*>(ret->value.get());
    STRATA_CHECK(add->op == BinaryOp::Add);
    STRATA_CHECK(add->lhs->kind == NodeKind::IntLiteral);
    STRATA_CHECK(add->rhs->kind == NodeKind::Binary);
    auto* mul = static_cast<BinaryExpr*>(add->rhs.get());
    STRATA_CHECK(mul->op == BinaryOp::Mul);
}

STRATA_TEST(parser_var_decl)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f() { int x = 5; return x; }", diag);
    STRATA_CHECK(!diag.HasErrors());
    auto* block = static_cast<Block*>(mod->functions[0]->body.get());
    STRATA_CHECK(block->statements[0]->kind == NodeKind::VarDecl);
    auto* vd = static_cast<VarDeclStmt*>(block->statements[0].get());
    STRATA_CHECK(vd->name == "x");
    STRATA_CHECK(vd->init != nullptr);
    STRATA_CHECK(vd->init->kind == NodeKind::IntLiteral);
}

STRATA_TEST(parser_call_and_member)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f() { return add(1, 2); }", diag);
    STRATA_CHECK(!diag.HasErrors());
    auto* ret = SingleReturn(mod.get());
    STRATA_CHECK(ret->value->kind == NodeKind::Call);
    auto* call = static_cast<CallExpr*>(ret->value.get());
    STRATA_CHECK(call->callee == "add");
    STRATA_CHECK_EQ(call->args.size(), (std::size_t)2);
}

STRATA_TEST(parser_recovers_from_error)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f( { }", diag);
    STRATA_CHECK(diag.HasErrors());
    STRATA_CHECK(diag.ErrorCount() >= 1);
}

STRATA_TEST(parser_if_else)
{
    DiagnosticEngine diag;
    auto mod = ParseModule("int f(int a) { if (a) { return 1; } else { return 2; } }", diag);
    STRATA_CHECK(!diag.HasErrors());
    auto* block = static_cast<Block*>(mod->functions[0]->body.get());
    STRATA_CHECK(block->statements[0]->kind == NodeKind::If);
    auto* ifn = static_cast<IfStmt*>(block->statements[0].get());
    STRATA_CHECK(ifn->thenBranch != nullptr);
    STRATA_CHECK(ifn->elseBranch != nullptr);
}
