#include "strata/AST/AST.h"
#include "strata/Codegen/CodegenBackend.h"

#include <sstream>

namespace strata
{

namespace
{

const char* UnaryOpSpelling(UnaryOp op) noexcept
{
    switch (op)
    {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Pos:
        return "+";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::BitNot:
        return "~";
    }

    return "?";
}

const char* BinaryOpSpelling(BinaryOp op) noexcept
{
    switch (op)
    {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::BitXor:
        return "^";
    case BinaryOp::Shl:
        return "<<";
    case BinaryOp::Shr:
        return ">>";
    case BinaryOp::EqEq:
        return "==";
    case BinaryOp::NotEq:
        return "!=";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::LtEq:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::GtEq:
        return ">=";
    case BinaryOp::LogicAnd:
        return "&&";
    case BinaryOp::LogicOr:
        return "||";
    }

    return "?";
}

const char* AssignOpSpelling(AssignOp op) noexcept
{
    switch (op)
    {
    case AssignOp::Assign:
        return "=";
    case AssignOp::PlusEq:
        return "+=";
    case AssignOp::MinusEq:
        return "-=";
    case AssignOp::StarEq:
        return "*=";
    case AssignOp::SlashEq:
        return "/=";
    case AssignOp::PercentEq:
        return "%=";
    }

    return "=";
}

void Dump(Node* n, int indent, std::ostringstream& out);

void Pad(int indent, std::ostringstream& out)
{
    for (int i = 0; i < indent; ++i)
    {
        out.put(' ');
    }
}

void Dump(Node* n, int indent, std::ostringstream& out)
{
    if (!n)
    {
        out << "(null)";
        return;
    }

    Pad(indent, out);
    switch (n->kind)
    {
    case NodeKind::Module:
    {
        auto* module = static_cast<Module*>(n);
        out << "module " << module->name << "\n";
        for (auto& s : module->structs)
        {
            Dump(s.get(), indent + 2, out);
        }

        for (auto& h : module->handles)
        {
            Dump(h.get(), indent + 2, out);
        }

        for (auto& func : module->functions)
        {
            Dump(func.get(), indent + 2, out);
        }

        return;
    }

    case NodeKind::Handle:
    {
        auto* handleDecl = static_cast<HandleDecl*>(n);
        out << "handle " << handleDecl->name << "  ; opaque\n";
        return;
    }

    case NodeKind::Struct:
    {
        auto* structDecl = static_cast<StructDecl*>(n);
        out << "struct " << structDecl->name << " {\n";
        for (const auto& field : structDecl->fields)
        {
            Pad(indent + 4, out);
            out << field.type.name << " " << field.name << "\n";
        }

        Pad(indent + 2, out);
        out << "}\n";
        return;
    }

    case NodeKind::Function:
    {
        auto* functionDecl = static_cast<FunctionDecl*>(n);
        out << "fn ";
        if (functionDecl->isExtern)
        {
            out << "extern ";
        }

        out << functionDecl->returnType.name << " " << functionDecl->name << "(";
        for (std::size_t i = 0; i < functionDecl->params.size(); ++i)
        {
            if (i)
            {
                out << ", ";
            }

            auto& param = functionDecl->params[i];
            out << ParamModSpelling(param->mod);
            if (param->mod != ParamMod::None)
            {
                out << " ";
            }

            out << param->type.name << " " << param->name;
        }

        out << ")";
        if (!functionDecl->body)
        {
            out << ";\n";
            return;
        }

        out << "\n";
        Dump(functionDecl->body.get(), indent + 2, out);
        return;
    }

    case NodeKind::Block:
    {
        auto* block = static_cast<Block*>(n);
        out << "{\n";
        for (auto& stmt : block->statements)
        {
            Dump(stmt.get(), indent + 2, out);
        }

        Pad(indent, out);
        out << "}\n";
        return;
    }

    case NodeKind::Return:
    {
        auto* returnStmt = static_cast<ReturnStmt*>(n);
        out << "return";
        if (returnStmt->value)
        {
            out << " ";
            Dump(returnStmt->value.get(), 0, out);
        }
        else
        {
            out << "\n";
        }

        return;
    }

    case NodeKind::If:
    {
        auto* i = static_cast<IfStmt*>(n);
        out << "if ";
        Dump(i->condition.get(), 0, out);
        out << "\n";
        Dump(i->thenBranch.get(), indent + 2, out);
        if (i->elseBranch)
        {
            Pad(indent, out);
            out << "else\n";
            Dump(i->elseBranch.get(), indent + 2, out);
        }

        return;
    }

    case NodeKind::While:
    {
        auto* w = static_cast<WhileStmt*>(n);
        out << "while ";
        Dump(w->condition.get(), 0, out);
        out << "\n";
        Dump(w->body.get(), indent + 2, out);
        return;
    }

    case NodeKind::For:
    {
        auto* forStmt = static_cast<ForStmt*>(n);
        out << "for (";
        if (forStmt->init)
        {
            Dump(forStmt->init.get(), 0, out);
        }
        else
        {
            out << "; ";
        }

        if (forStmt->condition)
        {
            Dump(forStmt->condition.get(), 0, out);
        }

        out << "; ";
        if (forStmt->update)
        {
            Dump(forStmt->update.get(), 0, out);
        }

        out << ")\n";
        Dump(forStmt->body.get(), indent + 2, out);
        return;
    }

    case NodeKind::VarDecl:
    {
        auto* varDecl = static_cast<VarDeclStmt*>(n);
        out << varDecl->type.name << " " << varDecl->name;
        if (varDecl->init)
        {
            out << " = ";
            Dump(varDecl->init.get(), 0, out);
        }
        else
        {
            out << "\n";
        }

        return;
    }

    case NodeKind::ExprStmt:
    {
        auto* expr = static_cast<ExprStmt*>(n);
        if (expr->expr)
        {
            Dump(expr->expr.get(), 0, out);
        }
        else
        {
            out << ";\n";
        }

        return;
    }

    case NodeKind::Break:
        out << "break\n";
        return;
    case NodeKind::Continue:
        out << "continue\n";
        return;
    case NodeKind::IntLiteral:
    {
        out << static_cast<IntLiteral*>(n)->value;
        if (static_cast<IntLiteral*>(n)->isUnsigned)
        {
            out << "u";
        }

        out << "\n";
        return;
    }

    case NodeKind::FloatLiteral:
        out << static_cast<FloatLiteral*>(n)->value << "\n";
        return;
    case NodeKind::BoolLiteral:
        out << (static_cast<BoolLiteral*>(n)->value ? "true\n" : "false\n");
        return;
    case NodeKind::Ident:
        out << static_cast<IdentExpr*>(n)->name << "\n";
        return;
    case NodeKind::Unary:
    {
        auto* unaryExpr = static_cast<UnaryExpr*>(n);
        out << "(" << UnaryOpSpelling(unaryExpr->op) << " ";
        Dump(unaryExpr->operand.get(), 0, out);
        return; // operand's trailing newline closes the paren-line
    }

    case NodeKind::Binary:
    {
        auto* binaryExpr = static_cast<BinaryExpr*>(n);
        out << "(" << BinaryOpSpelling(binaryExpr->op) << " ";
        Dump(binaryExpr->lhs.get(), 0, out);
        Pad(0, out);
        Dump(binaryExpr->rhs.get(), 0, out);
        return;
    }

    case NodeKind::Assign:
    {
        auto* assignExpr = static_cast<AssignExpr*>(n);
        out << "(" << AssignOpSpelling(assignExpr->op) << " ";
        Dump(assignExpr->target.get(), 0, out);
        Dump(assignExpr->value.get(), 0, out);
        return;
    }

    case NodeKind::Call:
    {
        auto* callExpr = static_cast<CallExpr*>(n);
        out << "(call " << callExpr->callee;
        for (auto& arg : callExpr->args)
        {
            out << " ";
            Dump(arg.get(), 0, out);
        }

        return;
    }

    case NodeKind::Member:
    {
        auto* memberExpr = static_cast<MemberExpr*>(n);
        out << "(. " << memberExpr->member << " ";
        Dump(memberExpr->base.get(), 0, out);
        return;
    }

    case NodeKind::StructInit:
    {
        auto* si = static_cast<StructInitExpr*>(n);
        out << "(struct-init " << si->typeName;
        for (auto& field : si->fields)
        {
            out << " ";
            if (!field.name.empty())
            {
                out << "(." << field.name << " ";
                Dump(field.value.get(), 0, out);
                out << ")";
            }
            else
            {
                Dump(field.value.get(), 0, out);
            }
        }

        return;
    }

    case NodeKind::Param:
        out << "(param)\n";
        return;
    }

    out << "(unknown node)\n";
}

} // namespace

std::string DumpAst(const Module& mod)
{
    std::ostringstream out;
    Dump(const_cast<Module*>(&mod), 0, out);
    return out.str();
}

} // namespace strata
