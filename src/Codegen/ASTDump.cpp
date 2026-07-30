#include "strata/AST/AST.h"
#include "strata/Codegen/CodegenBackend.h"

#include <sstream>

namespace strata {

namespace {

const char* unaryOpSpelling(UnaryOp op) noexcept {
    switch (op) {
        case UnaryOp::Neg: return "-";
        case UnaryOp::Pos: return "+";
        case UnaryOp::Not: return "!";
        case UnaryOp::BitNot: return "~";
    }
    return "?";
}

const char* binaryOpSpelling(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::Shl: return "<<";
        case BinaryOp::Shr: return ">>";
        case BinaryOp::EqEq: return "==";
        case BinaryOp::NotEq: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::LtEq: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::GtEq: return ">=";
        case BinaryOp::LogicAnd: return "&&";
        case BinaryOp::LogicOr: return "||";
    }
    return "?";
}

const char* assignOpSpelling(AssignOp op) noexcept {
    switch (op) {
        case AssignOp::Assign: return "=";
        case AssignOp::PlusEq: return "+=";
        case AssignOp::MinusEq: return "-=";
        case AssignOp::StarEq: return "*=";
        case AssignOp::SlashEq: return "/=";
        case AssignOp::PercentEq: return "%=";
    }
    return "=";
}

void dump(Node* n, int indent, std::ostringstream& out);

void pad(int indent, std::ostringstream& out) {
    for (int i = 0; i < indent; ++i) out.put(' ');
}

void dump(Node* n, int indent, std::ostringstream& out) {
    if (!n) { out << "(null)"; return; }
    pad(indent, out);
    switch (n->kind) {
        case NodeKind::Module: {
            auto m = static_cast<Module*>(n);
            out << "module " << m->name << "\n";
            for (auto& f : m->functions) dump(f.get(), indent + 2, out);
            return;
        }
        case NodeKind::Function: {
            auto f = static_cast<FunctionDecl*>(n);
            out << "fn " << f->returnType.name << " " << f->name << "(";
            for (std::size_t i = 0; i < f->params.size(); ++i) {
                if (i) out << ", ";
                auto& p = f->params[i];
                out << paramModSpelling(p->mod);
                if (p->mod != ParamMod::None) out << " ";
                out << p->type.name << " " << p->name;
            }
            out << ")";
            if (!f->body) { out << ";\n"; return; }
            out << "\n";
            dump(f->body.get(), indent + 2, out);
            return;
        }
        case NodeKind::Block: {
            auto b = static_cast<Block*>(n);
            out << "{\n";
            for (auto& s : b->statements) dump(s.get(), indent + 2, out);
            pad(indent, out);
            out << "}\n";
            return;
        }
        case NodeKind::Return: {
            auto r = static_cast<ReturnStmt*>(n);
            out << "return";
            if (r->value) {
                out << " ";
                dump(r->value.get(), 0, out);
            } else {
                out << "\n";
            }
            return;
        }
        case NodeKind::If: {
            auto i = static_cast<IfStmt*>(n);
            out << "if ";
            dump(i->condition.get(), 0, out);
            out << "\n";
            dump(i->thenBranch.get(), indent + 2, out);
            if (i->elseBranch) {
                pad(indent, out);
                out << "else\n";
                dump(i->elseBranch.get(), indent + 2, out);
            }
            return;
        }
        case NodeKind::While: {
            auto w = static_cast<WhileStmt*>(n);
            out << "while ";
            dump(w->condition.get(), 0, out);
            out << "\n";
            dump(w->body.get(), indent + 2, out);
            return;
        }
        case NodeKind::VarDecl: {
            auto v = static_cast<VarDeclStmt*>(n);
            out << v->type.name << " " << v->name;
            if (v->init) {
                out << " = ";
                dump(v->init.get(), 0, out);
            } else {
                out << "\n";
            }
            return;
        }
        case NodeKind::ExprStmt: {
            auto e = static_cast<ExprStmt*>(n);
            if (e->expr) dump(e->expr.get(), 0, out); else out << ";\n";
            return;
        }
        case NodeKind::Break:    out << "break\n"; return;
        case NodeKind::Continue: out << "continue\n"; return;
        case NodeKind::IntLiteral: {
            out << static_cast<IntLiteral*>(n)->value;
            if (static_cast<IntLiteral*>(n)->isUnsigned) out << "u";
            out << "\n";
            return;
        }
        case NodeKind::FloatLiteral:
            out << static_cast<FloatLiteral*>(n)->value << "\n"; return;
        case NodeKind::BoolLiteral:
            out << (static_cast<BoolLiteral*>(n)->value ? "true\n" : "false\n"); return;
        case NodeKind::Ident:
            out << static_cast<IdentExpr*>(n)->name << "\n"; return;
        case NodeKind::Unary: {
            auto u = static_cast<UnaryExpr*>(n);
            out << "(" << unaryOpSpelling(u->op) << " ";
            dump(u->operand.get(), 0, out);
            return; // operand's trailing newline closes the paren-line
        }
        case NodeKind::Binary: {
            auto b = static_cast<BinaryExpr*>(n);
            out << "(" << binaryOpSpelling(b->op) << " ";
            dump(b->lhs.get(), 0, out);
            pad(0, out);
            dump(b->rhs.get(), 0, out);
            return;
        }
        case NodeKind::Assign: {
            auto a = static_cast<AssignExpr*>(n);
            out << "(" << assignOpSpelling(a->op) << " ";
            dump(a->target.get(), 0, out);
            dump(a->value.get(), 0, out);
            return;
        }
        case NodeKind::Call: {
            auto c = static_cast<CallExpr*>(n);
            out << "(call " << c->callee;
            for (auto& a : c->args) { out << " "; dump(a.get(), 0, out); }
            return;
        }
        case NodeKind::Member: {
            auto mem = static_cast<MemberExpr*>(n);
            out << "(. " << mem->member << " ";
            dump(mem->base.get(), 0, out);
            return;
        }
        case NodeKind::Param: out << "(param)\n"; return;
    }
    out << "(unknown node)\n";
}

} // namespace

std::string dumpAST(const Module& mod) {
    std::ostringstream out;
    dump(const_cast<Module*>(&mod), 0, out);
    return out.str();
}

} // namespace strata
