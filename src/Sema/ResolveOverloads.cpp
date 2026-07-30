// Strata compiler: overload resolution implementation.
#include "strata/Sema/ResolveOverloads.h"
#include "../Codegen/TypeRegistry.h"

#include <climits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

namespace {

using VarScope = std::map<std::string, std::string>; // variable name -> source type

bool isNumeric(std::string_view t) {
    return t == "int" || t == "uint" || t == "half" || t == "float" || t == "double";
}

class Resolver {
public:
    Resolver(Module& m, DiagnosticEngine& d) : mod_(m), diag_(d) { registry_.build(m); }

    void run() {
        // Group functions by source name.
        byName_.clear();
        for (auto& f : mod_.functions) byName_[f->name].push_back(f.get());

        // Assign mangled IR names.
        byMangled_.clear();
        for (auto& [name, vec] : byName_) {
            bool overloaded = vec.size() > 1;
            for (auto* f : vec) {
                if (overloaded && f->isExtern) {
                    diag_.error(f->range,
                                "extern function '" + name + "' cannot be overloaded");
                }
                f->mangledName = overloaded ? mangle(*f) : f->name;
                if (byMangled_.count(f->mangledName)) {
                    diag_.error(f->range,
                                "duplicate function signature for '" + name + "'");
                }
                byMangled_[f->mangledName] = f;
            }
        }

        // Resolve every call site.
        for (auto& f : mod_.functions) {
            if (!f->body) continue;
            VarScope scope;
            for (auto& p : f->params) scope[p->name] = p->type.name;
            walkBlock(*static_cast<Block*>(f->body.get()), scope);
        }

        // Extern functions cross the host boundary. Structs must cross by
        // pointer (by-value struct passing is ABI-fragile), so an extern struct
        // parameter must declare in/out/inout, and an extern may not return a
        // struct by value (use an out parameter instead).
        for (auto& f : mod_.functions) {
            if (!f->isExtern) continue;
            for (auto& p : f->params) {
                if (isDefinedStruct(p->type.name) && p->mod == ParamMod::None) {
                    diag_.error(p->range,
                                "extern struct parameter '" + p->name +
                                    "' must be declared in/out/inout (it crosses the host "
                                    "boundary by pointer)");
                }
            }
            if (isDefinedStruct(f->returnType.name)) {
                diag_.error(f->range,
                            "extern function cannot return a struct by value; use an out "
                            "parameter");
            }
        }
    }

private:
    Module& mod_;
    DiagnosticEngine& diag_;
    TypeRegistry registry_;
    std::map<std::string, std::vector<FunctionDecl*>> byName_;
    std::map<std::string, FunctionDecl*> byMangled_;

    static std::string mangle(const FunctionDecl& f) {
        std::string s = f.name;
        for (auto& p : f.params) { s += '$'; s += p->type.name; }
        return s;
    }

    // A user-defined struct with a known layout (not an opaque handle).
    bool isDefinedStruct(std::string_view name) const {
        return registry_.isUserType(name) && !registry_.isOpaque(name);
    }

    void walkBlock(Block& b, VarScope& scope) {
        for (auto& s : b.statements) walkStmt(s.get(), scope);
    }

    void walkStmt(Node* n, VarScope& scope) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Block: walkBlock(*static_cast<Block*>(n), scope); return;
            case NodeKind::VarDecl: {
                auto vd = static_cast<VarDeclStmt*>(n);
                if (vd->init) resolveExpr(vd->init.get(), scope);
                scope[vd->name] = vd->type.name;
                return;
            }
            case NodeKind::ExprStmt:
                resolveExpr(static_cast<ExprStmt*>(n)->expr.get(), scope); return;
            case NodeKind::Return: {
                auto r = static_cast<ReturnStmt*>(n);
                if (r->value) resolveExpr(r->value.get(), scope);
                return;
            }
            case NodeKind::If: {
                auto i = static_cast<IfStmt*>(n);
                resolveExpr(i->condition.get(), scope);
                walkStmt(i->thenBranch.get(), scope);
                if (i->elseBranch) walkStmt(i->elseBranch.get(), scope);
                return;
            }
            case NodeKind::While: {
                auto w = static_cast<WhileStmt*>(n);
                resolveExpr(w->condition.get(), scope);
                walkStmt(w->body.get(), scope);
                return;
            }
            case NodeKind::For: {
                auto fs = static_cast<ForStmt*>(n);
                if (fs->init) {
                    if (fs->init->kind == NodeKind::VarDecl) walkStmt(fs->init.get(), scope);
                    else resolveExpr(fs->init.get(), scope);
                }
                if (fs->condition) resolveExpr(fs->condition.get(), scope);
                if (fs->update) resolveExpr(fs->update.get(), scope);
                walkStmt(fs->body.get(), scope);
                return;
            }
            default: return;
        }
    }

    // Depth-first: resolve sub-expressions first, then resolve the call itself
    // (so argument types are known when overload resolution runs).
    void resolveExpr(Node* n, VarScope& scope) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::IntLiteral:
            case NodeKind::FloatLiteral:
            case NodeKind::BoolLiteral:
            case NodeKind::Ident:
                return;
            case NodeKind::Unary:
                resolveExpr(static_cast<UnaryExpr*>(n)->operand.get(), scope); return;
            case NodeKind::Binary: {
                auto b = static_cast<BinaryExpr*>(n);
                resolveExpr(b->lhs.get(), scope);
                resolveExpr(b->rhs.get(), scope);
                return;
            }
            case NodeKind::Assign: {
                auto a = static_cast<AssignExpr*>(n);
                resolveExpr(a->target.get(), scope);
                resolveExpr(a->value.get(), scope);
                return;
            }
            case NodeKind::Member:
                resolveExpr(static_cast<MemberExpr*>(n)->base.get(), scope); return;
            case NodeKind::Call: {
                auto c = static_cast<CallExpr*>(n);
                for (auto& a : c->args) resolveExpr(a.get(), scope);
                resolveCall(*c, scope);
                return;
            }
            default: return;
        }
    }

    void resolveCall(CallExpr& c, VarScope& scope) {
        auto it = byName_.find(c.callee);
        if (it == byName_.end() || it->second.empty()) return; // unknown -> codegen reports

        std::vector<std::string> argTypes;
        argTypes.reserve(c.args.size());
        for (auto& a : c.args) argTypes.push_back(inferType(a.get(), scope));

        FunctionDecl* best = nullptr;
        int bestScore = INT_MAX;
        bool ambiguous = false;
        for (auto* f : it->second) {
            if (f->params.size() != c.args.size()) continue;
            int score = 0;
            bool viable = true;
            for (std::size_t i = 0; i < c.args.size(); ++i) {
                int m = matchRank(argTypes[i], f->params[i]->type.name);
                if (m < 0) { viable = false; break; }
                score += m;
            }
            if (!viable) continue;
            if (score < bestScore) { bestScore = score; best = f; ambiguous = false; }
            else if (score == bestScore && best) ambiguous = true;
        }

        if (!best) {
            diag_.error(c.range, "no matching overload for '" + c.callee + "' with " +
                                  std::to_string(c.args.size()) + " argument(s)");
            return;
        }
        if (ambiguous) {
            diag_.error(c.range, "ambiguous call to overload '" + c.callee + "'");
        }
        c.callee = best->mangledName;
        c.resolvedDecl = best;
    }

    // 0 = exact, 1 = numeric conversion, -1 = no match.
    static int matchRank(std::string_view argType, std::string_view paramType) {
        if (argType.empty()) return 0; // unknown argument type: treat as a wildcard
        if (argType == paramType) return 0;
        if (isNumeric(argType) && isNumeric(paramType)) return 1;
        return -1;
    }

    std::string inferType(Node* n, VarScope& scope) {
        if (!n) return "";
        switch (n->kind) {
            case NodeKind::IntLiteral:
                return static_cast<IntLiteral*>(n)->isUnsigned ? "uint" : "int";
            case NodeKind::FloatLiteral: return "float";
            case NodeKind::BoolLiteral: return "bool";
            case NodeKind::Ident: {
                auto it = scope.find(static_cast<IdentExpr*>(n)->name);
                return it == scope.end() ? "" : it->second;
            }
            case NodeKind::Unary: {
                auto u = static_cast<UnaryExpr*>(n);
                return u->op == UnaryOp::Not ? "bool" : inferType(u->operand.get(), scope);
            }
            case NodeKind::Binary: {
                auto b = static_cast<BinaryExpr*>(n);
                switch (b->op) {
                    case BinaryOp::EqEq: case BinaryOp::NotEq:
                    case BinaryOp::Lt: case BinaryOp::LtEq:
                    case BinaryOp::Gt: case BinaryOp::GtEq:
                    case BinaryOp::LogicAnd: case BinaryOp::LogicOr:
                        return "bool";
                    default: {
                        std::string lt = inferType(b->lhs.get(), scope);
                        std::string rt = inferType(b->rhs.get(), scope);
                        if (lt == "double" || rt == "double") return "double";
                        if (lt == "half" || rt == "half") return "half";
                        if (lt == "float" || rt == "float") return "float";
                        if (lt == "uint" || rt == "uint") return "uint";
                        return "int";
                    }
                }
            }
            case NodeKind::Assign:
                return inferType(static_cast<AssignExpr*>(n)->target.get(), scope);
            case NodeKind::Member: {
                auto m = static_cast<MemberExpr*>(n);
                std::string baseName = inferType(m->base.get(), scope);
                const auto* st = registry_.find(baseName);
                if (!st) return "";
                int idx = registry_.fieldIndex(baseName, m->member);
                if (idx < 0) return "";
                return st->fields[static_cast<std::size_t>(idx)].type.name;
            }
            case NodeKind::Call: {
                auto c = static_cast<CallExpr*>(n);
                if (c->resolvedDecl) return c->resolvedDecl->returnType.name;
                if (registry_.isUserType(c->callee)) return c->callee; // constructor
                return "";
            }
            default: return "";
        }
    }
};

} // namespace

void resolveOverloads(Module& mod, DiagnosticEngine& diag) {
    Resolver r(mod, diag);
    r.run();
}

} // namespace strata
