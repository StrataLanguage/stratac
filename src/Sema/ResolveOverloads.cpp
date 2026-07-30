// Strata compiler: overload resolution implementation.
#include "strata/Sema/ResolveOverloads.h"
#include "../Codegen/TypeRegistry.h"

#include <climits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace strata
{

namespace
{

using VarScope = std::map<std::string, std::string>; // variable name -> source type

bool IsNumeric(std::string_view t)
{
    return t == "int" || t == "uint" || t == "half" || t == "float" || t == "double";
}

class Resolver
{
  public:
    Resolver(Module& m, DiagnosticEngine& d) : m_mod(m), m_diag(d)
    {
        m_registry.Build(m);
    }

    void Run()
    {
        // Group functions by source name.
        m_byName.clear();

        for (auto& f : m_mod.functions)
        {
            m_byName[f->name].push_back(f.get());
        }

        // Assign mangled IR names.
        m_byMangled.clear();
        for (auto& [name, vec] : m_byName)
        {
            bool overloaded = vec.size() > 1;
            for (auto* f : vec)
            {
                if (overloaded && f->isExtern)
                {
                    m_diag.Error(f->range, "extern function '" + name + "' cannot be overloaded");
                }

                f->mangledName = overloaded ? Mangle(*f) : f->name;
                if (m_byMangled.contains(f->mangledName))
                {
                    m_diag.Error(f->range, "duplicate function signature for '" + name + "'");
                }

                m_byMangled[f->mangledName] = f;
            }
        }

        // Resolve every call site.
        for (auto& f : m_mod.functions)
        {
            if (!f->body)
            {
                continue;
            }

            VarScope scope;
            for (auto& p : f->params)
            {
                scope[p->name] = p->type.name;
            }

            WalkBlock(*static_cast<Block*>(f->body.get()), scope);
        }

        // Struct parameters are always passed by reference, so every struct
        // parameter (on any function) must declare in/out/inout. An extern may
        // not return a struct by value (use an out parameter instead).
        for (auto& f : m_mod.functions)
        {
            for (auto& p : f->params)
            {
                if (IsDefinedStruct(p->type.name) && p->mod == ParamMod::None)
                {
                    m_diag.Error(p->range, "struct parameter '" + p->name +
                                               "' must be declared in/out/inout (structs are passed by "
                                               "reference; copy with '" +
                                               p->type.name + " " + p->name + "_copy = " + p->name +
                                               ";' if you need a local value)");
                }
            }

            if (f->isExtern && IsDefinedStruct(f->returnType.name))
            {
                m_diag.Error(f->range, "extern function cannot return a struct by value; use an out "
                                       "parameter");
            }
        }
    }

  private:
    Module& m_mod;
    DiagnosticEngine& m_diag;
    TypeRegistry m_registry;
    std::map<std::string, std::vector<FunctionDecl*>> m_byName;
    std::map<std::string, FunctionDecl*> m_byMangled;

    static std::string Mangle(const FunctionDecl& f)
    {
        std::string s = f.name;
        for (const auto& p : f.params)
        {
            s += '$';
            s += p->type.name;
        }

        return s;
    }

    // A user-defined struct with a known layout (not an opaque handle).
    bool IsDefinedStruct(std::string_view name) const
    {
        return m_registry.IsUserType(name) && !m_registry.IsOpaque(name);
    }

    void WalkBlock(Block& b, VarScope& scope)
    {
        for (auto& s : b.statements)
        {
            WalkStmt(s.get(), scope);
        }
    }

    void WalkStmt(Node* n, VarScope& scope)
    {
        if (!n)
        {
            return;
        }

        switch (n->kind)
        {
        case NodeKind::Block:
            WalkBlock(*static_cast<Block*>(n), scope);
            return;
        case NodeKind::VarDecl:
        {
            auto* vd = static_cast<VarDeclStmt*>(n);
            if (vd->init)
            {
                ResolveExpr(vd->init.get(), scope);
            }

            scope[vd->name] = vd->type.name;
            return;
        }

        case NodeKind::ExprStmt:
            ResolveExpr(static_cast<ExprStmt*>(n)->expr.get(), scope);
            return;
        case NodeKind::Return:
        {
            auto* r = static_cast<ReturnStmt*>(n);
            if (r->value)
            {
                ResolveExpr(r->value.get(), scope);
            }

            return;
        }

        case NodeKind::If:
        {
            auto* i = static_cast<IfStmt*>(n);
            ResolveExpr(i->condition.get(), scope);
            WalkStmt(i->thenBranch.get(), scope);
            if (i->elseBranch)
            {
                WalkStmt(i->elseBranch.get(), scope);
            }

            return;
        }

        case NodeKind::While:
        {
            auto* w = static_cast<WhileStmt*>(n);
            ResolveExpr(w->condition.get(), scope);
            WalkStmt(w->body.get(), scope);
            return;
        }

        case NodeKind::For:
        {
            auto* fs = static_cast<ForStmt*>(n);
            if (fs->init)
            {
                if (fs->init->kind == NodeKind::VarDecl)
                {
                    WalkStmt(fs->init.get(), scope);
                }
                else
                {
                    ResolveExpr(fs->init.get(), scope);
                }
            }

            if (fs->condition)
            {
                ResolveExpr(fs->condition.get(), scope);
            }

            if (fs->update)
            {
                ResolveExpr(fs->update.get(), scope);
            }

            WalkStmt(fs->body.get(), scope);
            return;
        }

        default:
            return;
        }
    }

    // Depth-first: resolve sub-expressions first, then resolve the call itself
    // (so argument types are known when overload resolution runs).
    void ResolveExpr(Node* n, VarScope& scope)
    {
        if (!n)
        {
            return;
        }

        switch (n->kind)
        {
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::Ident:
            return;
        case NodeKind::Unary:
            ResolveExpr(static_cast<UnaryExpr*>(n)->operand.get(), scope);
            return;
        case NodeKind::Binary:
        {
            auto* b = static_cast<BinaryExpr*>(n);
            ResolveExpr(b->lhs.get(), scope);
            ResolveExpr(b->rhs.get(), scope);
            return;
        }

        case NodeKind::Assign:
        {
            auto* a = static_cast<AssignExpr*>(n);
            ResolveExpr(a->target.get(), scope);
            ResolveExpr(a->value.get(), scope);
            return;
        }

        case NodeKind::Member:
        {
            auto* m = static_cast<MemberExpr*>(n);
            ResolveExpr(m->base.get(), scope);
            std::string baseName = InferType(m->base.get(), scope);
            if (m_registry.IsOpaque(baseName))
            {
                m_diag.Error(m->range, "cannot access member '" + m->member + "' of opaque handle '" + baseName + "'");
            }

            return;
        }

        case NodeKind::Call:
        {
            auto* c = static_cast<CallExpr*>(n);
            for (auto& a : c->args)
            {
                ResolveExpr(a.get(), scope);
            }

            ResolveCall(*c, scope);
            return;
        }

        default:
            return;
        }
    }

    void ResolveCall(CallExpr& c, VarScope& scope)
    {
        auto it = m_byName.find(c.callee);
        if (it == m_byName.end() || it->second.empty())
        {
            return; // unknown -> codegen reports
        }

        std::vector<std::string> argTypes;
        argTypes.reserve(c.args.size());
        for (auto& a : c.args)
        {
            argTypes.push_back(InferType(a.get(), scope));
        }

        FunctionDecl* best = nullptr;
        int bestScore = INT_MAX;
        bool ambiguous = false;
        for (auto* f : it->second)
        {
            if (f->params.size() != c.args.size())
            {
                continue;
            }

            int score = 0;
            bool viable = true;
            for (std::size_t i = 0; i < c.args.size(); ++i)
            {
                int m = MatchRank(argTypes[i], f->params[i]->type.name);
                if (m < 0)
                {
                    viable = false;
                    break;
                }

                score += m;
            }

            if (!viable)
            {
                continue;
            }

            if (score < bestScore)
            {
                bestScore = score;
                best = f;
                ambiguous = false;
            }
            else if (score == bestScore && best)
            {
                ambiguous = true;
            }
        }

        if (!best)
        {
            m_diag.Error(c.range, "no matching overload for '" + c.callee + "' with " + std::to_string(c.args.size()) +
                                      " argument(s)");
            return;
        }

        if (ambiguous)
        {
            m_diag.Error(c.range, "ambiguous call to overload '" + c.callee + "'");
        }

        c.callee = best->mangledName;
        c.resolvedDecl = best;
    }

    // 0 = exact, 1 = numeric conversion, -1 = no match.
    static int MatchRank(std::string_view argType, std::string_view paramType)
    {
        if (argType.empty())
        {
            return 0; // unknown argument type: treat as a wildcard
        }

        if (argType == paramType)
        {
            return 0;
        }

        if (IsNumeric(argType) && IsNumeric(paramType))
        {
            return 1;
        }

        return -1;
    }

    std::string InferType(Node* n, VarScope& scope)
    {
        if (!n)
        {
            return "";
        }

        switch (n->kind)
        {
        case NodeKind::IntLiteral:
            return static_cast<IntLiteral*>(n)->isUnsigned ? "uint" : "int";
        case NodeKind::FloatLiteral:
            return "float";
        case NodeKind::BoolLiteral:
            return "bool";
        case NodeKind::Ident:
        {
            auto it = scope.find(static_cast<IdentExpr*>(n)->name);
            return it == scope.end() ? "" : it->second;
        }

        case NodeKind::Unary:
        {
            auto* u = static_cast<UnaryExpr*>(n);
            return u->op == UnaryOp::Not ? "bool" : InferType(u->operand.get(), scope);
        }

        case NodeKind::Binary:
        {
            auto* b = static_cast<BinaryExpr*>(n);
            switch (b->op)
            {
            case BinaryOp::EqEq:
            case BinaryOp::NotEq:
            case BinaryOp::Lt:
            case BinaryOp::LtEq:
            case BinaryOp::Gt:
            case BinaryOp::GtEq:
            case BinaryOp::LogicAnd:
            case BinaryOp::LogicOr:
                return "bool";
            default:
            {
                std::string lt = InferType(b->lhs.get(), scope);
                std::string rt = InferType(b->rhs.get(), scope);
                if (lt == "double" || rt == "double")
                {
                    return "double";
                }

                if (lt == "half" || rt == "half")
                {
                    return "half";
                }

                if (lt == "float" || rt == "float")
                {
                    return "float";
                }

                if (lt == "uint" || rt == "uint")
                {
                    return "uint";
                }

                return "int";
            }
            }
        }

        case NodeKind::Assign:
            return InferType(static_cast<AssignExpr*>(n)->target.get(), scope);
        case NodeKind::Member:
        {
            auto* m = static_cast<MemberExpr*>(n);
            std::string baseName = InferType(m->base.get(), scope);
            if (m_registry.IsOpaque(baseName))
            {
                m_diag.Error(m->range, "cannot access member '" + m->member + "' of opaque handle '" + baseName + "'");
                return "";
            }

            const auto* st = m_registry.Find(baseName);
            if (!st)
            {
                return "";
            }

            int idx = m_registry.FieldIndex(baseName, m->member);
            if (idx < 0)
            {
                return "";
            }

            return st->fields[static_cast<std::size_t>(idx)].type.name;
        }

        case NodeKind::Call:
        {
            auto* c = static_cast<CallExpr*>(n);
            if (c->resolvedDecl)
            {
                return c->resolvedDecl->returnType.name;
            }

            if (m_registry.IsUserType(c->callee))
            {
                return c->callee; // constructor
            }

            return "";
        }

        default:
            return "";
        }
    }
};

} // namespace

void ResolveOverloads(Module& mod, DiagnosticEngine& diag)
{
    Resolver r(mod, diag);
    r.Run();
}

} // namespace strata
