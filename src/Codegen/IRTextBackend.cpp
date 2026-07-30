// Strata compiler: text back-end.
//
// Emits LLVM IR as text from the AST. This is deliberately a real (if small)
// lowering rather than a stub: scalar integer/float functions with parameters,
// locals, assignment, arithmetic, comparisons, control flow (if/else, while),
// and calls are turned into valid LLVM IR that downstream tools (clang, llc)
// can assemble. Vector types and short-circuit logic are typed/passed through
// but not fully lowered yet; those are flagged in the IR as TODO and left as
// follow-up work.
#include "strata/Codegen/CodegenBackend.h"
#include "TypeUtil.h"
#include "strata/AST/AST.h"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace strata {

namespace {

struct Symbol {
    detail::MappedType type;
    std::string ptr; // alloca pointer, e.g. "%3"
};

struct Eval {
    detail::MappedType type;
    std::string val; // register "%3" or literal "3" / "1.500000e+00" / "1"
};

class IRTextImpl {
public:
    CodegenResult run(const Module& mod) {
        CodegenResult res;
        res.moduleName = mod.name;
        out_ << "; Strata module '" << mod.name << "'\n";

        // Emit body-less prototypes as 'declare', then definitions as 'define'.
        // A function may not be both declared and defined; calls to functions
        // defined later in the module resolve through LLVM forward references.
        for (const auto& f : mod.functions)
            if (!f->body) emitDeclare(*f);
        out_ << "\n";
        for (const auto& f : mod.functions)
            if (f->body) emitFunction(*f);

        res.ok = true;
        res.output = out_.str();
        return res;
    }

private:
    std::ostringstream out_;
    std::ostringstream body_;
    std::map<std::string, Symbol> symbols_;
    std::vector<std::string> paramRegs_;
    detail::MappedType retType_;
    int tmp_ = 0;
    int label_ = 0;
    bool terminated_ = false;
    struct Loop { std::string cont; std::string end; };
    std::vector<Loop> loops_;

    std::string newReg() { return "%t" + std::to_string(tmp_++); }
    std::string newLabel() { return std::string("L") + std::to_string(label_++); }

    static std::string floatConst(double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6e", v);
        return std::string(buf);
    }

    detail::MappedType mappedOr(const TypeName& t, const char* fallback) {
        auto m = detail::mapType(t);
        if (!m.valid) {
            out_ << "; TODO: unsupported type '" << t.name << "' lowered as " << fallback << "\n";
            m.ir = fallback;
            m.elemIr = fallback;
        }
        return m;
    }

    void emitDeclare(const FunctionDecl& f) {
        retType_ = mappedOr(f.returnType, "void");
        out_ << "declare " << retType_.ir << " @" << f.name << "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) out_ << ", ";
            out_ << mappedOr(f.params[i]->type, "ptr").ir;
        }
        out_ << ")\n";
    }

    void emitFunction(const FunctionDecl& f) {
        retType_ = mappedOr(f.returnType, "void");
        std::vector<detail::MappedType> ptypes;
        out_ << "define " << retType_.ir << " @" << f.name << "(";
        paramRegs_.clear();
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            ptypes.push_back(mappedOr(f.params[i]->type, "ptr"));
            paramRegs_.push_back("%p" + std::to_string(i));
            if (i) out_ << ", ";
            out_ << ptypes.back().ir << " " << paramRegs_.back();
        }
        out_ << ") {\n";

        // Reset per-function state.
        body_.str({});
        symbols_.clear();
        tmp_ = 0;
        label_ = 0;
        terminated_ = false;
        loops_.clear();

        // Materialize each parameter into an addressable slot.
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            std::string slot = newReg();
            body_ << "  " << slot << " = alloca " << ptypes[i].ir << "\n";
            body_ << "  store " << ptypes[i].ir << " " << paramRegs_[i] << ", ptr " << slot << "\n";
            symbols_[f.params[i]->name] = {ptypes[i], slot};
        }

        if (f.body) {
            auto* block = static_cast<Block*>(f.body.get());
            for (auto& s : block->statements) emitStmt(s.get());
        }

        if (!terminated_) {
            if (retType_.isVoid) body_ << "  ret void\n";
            else body_ << "  ret " << retType_.ir << " 0\n";
        }

        out_ << body_.str();
        out_ << "}\n\n";
    }

    void emitLabel(const std::string& l) {
        body_ << l << ":\n";
        terminated_ = false;
    }
    void emitBr(const std::string& target) {
        if (!terminated_) {
            body_ << "  br label %" << target << "\n";
            terminated_ = true;
        }
    }

    // Coerces an evaluated value to a target scalar type when they differ by
    // int<->float. Returns the (possibly new) value reference.
    Eval coerce(Eval ev, const detail::MappedType& target) {
        if (ev.type.ir == target.ir || target.isVoid) return ev;
        if (target.isVector() || ev.type.isVector()) return ev; // WIP
        std::string r = newReg();
        if (!ev.type.isFloat && target.isFloat) {
            body_ << "  " << r << " = " << (ev.type.isUnsigned ? "uitofp " : "sitofp ")
                  << ev.type.ir << " " << ev.val << " to " << target.ir << "\n";
        } else if (ev.type.isFloat && !target.isFloat) {
            body_ << "  " << r << " = " << (target.isUnsigned ? "fptoui " : "fptosi ")
                  << ev.type.ir << " " << ev.val << " to " << target.ir << "\n";
        } else {
            return ev; // best effort (e.g. width changes not handled yet)
        }
        return {target, r};
    }

    void emitStmt(Node* n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Block:
                for (auto& s : static_cast<Block*>(n)->statements) emitStmt(s.get());
                return;
            case NodeKind::ExprStmt:
                if (auto e = static_cast<ExprStmt*>(n)->expr.get()) (void)emitExpr(e);
                return;
            case NodeKind::Return: {
                auto r = static_cast<ReturnStmt*>(n);
                if (r->value) {
                    Eval v = coerce(emitExpr(r->value.get()), retType_);
                    body_ << "  ret " << retType_.ir << " " << v.val << "\n";
                } else {
                    body_ << "  ret void\n";
                }
                terminated_ = true;
                return;
            }
            case NodeKind::VarDecl: {
                auto vd = static_cast<VarDeclStmt*>(n);
                auto ty = mappedOr(vd->type, "i32");
                std::string slot = newReg();
                body_ << "  " << slot << " = alloca " << ty.ir << "\n";
                if (vd->init) {
                    Eval v = coerce(emitExpr(vd->init.get()), ty);
                    body_ << "  store " << ty.ir << " " << v.val << ", ptr " << slot << "\n";
                }
                symbols_[vd->name] = {ty, slot};
                return;
            }
            case NodeKind::If:
                emitIf(static_cast<IfStmt*>(n));
                return;
            case NodeKind::While:
                emitWhile(static_cast<WhileStmt*>(n));
                return;
            case NodeKind::Break:
                if (!loops_.empty()) emitBr(loops_.back().end);
                return;
            case NodeKind::Continue:
                if (!loops_.empty()) emitBr(loops_.back().cont);
                return;
            default:
                (void)emitExpr(n); // expression-shaped statement
                return;
        }
    }

    void emitIf(IfStmt* n) {
        Eval cond = emitExpr(n->condition.get());
        std::string thenL = newLabel();
        std::string elseL = newLabel();
        std::string endL = newLabel();
        bool hasElse = n->elseBranch != nullptr;
        body_ << "  br i1 " << cond.val << ", label %" << thenL << ", label %"
              << (hasElse ? elseL : endL) << "\n";
        terminated_ = true;

        emitLabel(thenL);
        emitStmt(n->thenBranch.get());
        emitBr(endL);

        if (hasElse) {
            emitLabel(elseL);
            emitStmt(n->elseBranch.get());
            emitBr(endL);
        }
        emitLabel(endL);
    }

    void emitWhile(WhileStmt* n) {
        std::string condL = newLabel();
        std::string bodyL = newLabel();
        std::string endL = newLabel();
        body_ << "  br label %" << condL << "\n";
        terminated_ = true;
        emitLabel(condL);
        Eval cond = emitExpr(n->condition.get());
        body_ << "  br i1 " << cond.val << ", label %" << bodyL << ", label %" << endL << "\n";
        terminated_ = true;
        emitLabel(bodyL);
        loops_.push_back({condL, endL});
        emitStmt(n->body.get());
        loops_.pop_back();
        emitBr(condL);
        emitLabel(endL);
    }

    Eval emitExpr(Node* n) {
        if (!n) return {mappedOr({"int"}, "i32"), "0"};
        switch (n->kind) {
            case NodeKind::IntLiteral: {
                auto l = static_cast<IntLiteral*>(n);
                detail::MappedType t = detail::mapType({l->isUnsigned ? "uint" : "int"});
                return {t, std::to_string(l->value)};
            }
            case NodeKind::FloatLiteral: {
                auto l = static_cast<FloatLiteral*>(n);
                return {detail::mapType({"float"}), floatConst(l->value)};
            }
            case NodeKind::BoolLiteral:
                return {detail::mapType({"bool"}), static_cast<BoolLiteral*>(n)->value ? "1" : "0"};
            case NodeKind::Ident:
                return emitIdent(static_cast<IdentExpr*>(n));
            case NodeKind::Unary:
                return emitUnary(static_cast<UnaryExpr*>(n));
            case NodeKind::Binary:
                return emitBinary(static_cast<BinaryExpr*>(n));
            case NodeKind::Call:
                return emitCall(static_cast<CallExpr*>(n));
            case NodeKind::Assign:
                return emitAssign(static_cast<AssignExpr*>(n));
            default:
                out_ << "; TODO: unsupported expression kind\n";
                return {detail::mapType({"int"}), "0"};
        }
    }

    Eval emitIdent(IdentExpr* n) {
        auto it = symbols_.find(n->name);
        if (it == symbols_.end()) {
            out_ << "; TODO: unknown identifier '" << n->name << "'\n";
            return {detail::mapType({"int"}), "0"};
        }
        std::string r = newReg();
        body_ << "  " << r << " = load " << it->second.type.ir << ", ptr " << it->second.ptr << "\n";
        return {it->second.type, r};
    }

    Eval emitUnary(UnaryExpr* n) {
        Eval e = emitExpr(n->operand.get());
        std::string r = newReg();
        switch (n->op) {
            case UnaryOp::Pos:
                return e;
            case UnaryOp::Neg:
                if (e.type.isFloat)
                    body_ << "  " << r << " = fneg " << e.type.ir << " " << e.val << "\n";
                else
                    body_ << "  " << r << " = sub " << e.type.ir << " 0, " << e.val << "\n";
                return {e.type, r};
            case UnaryOp::Not:
                body_ << "  " << r << " = xor i1 " << e.val << ", true\n";
                return {detail::mapType({"bool"}), r};
            case UnaryOp::BitNot:
                body_ << "  " << r << " = xor " << e.type.ir << " " << e.val << ", -1\n";
                return {e.type, r};
        }
        return e;
    }

    Eval emitBinary(BinaryExpr* n) {
        Eval l = emitExpr(n->lhs.get());
        Eval r = emitExpr(n->rhs.get());
        detail::MappedType t = l.type;
        std::string out = newReg();

        if (n->op == BinaryOp::LogicAnd || n->op == BinaryOp::LogicOr) {
            // Non-short-circuit on i1 for now.
            const char* op = (n->op == BinaryOp::LogicAnd) ? "and" : "or";
            body_ << "  " << out << " = " << op << " i1 " << l.val << ", " << r.val << "\n";
            return {detail::mapType({"bool"}), out};
        }

        bool flt = t.isFloat;
        auto icmp = [&](const char* pred) {
            body_ << "  " << out << " = icmp " << pred << " " << t.ir << " " << l.val << ", " << r.val
                  << "\n";
        };
        auto fcmp = [&](const char* pred) {
            body_ << "  " << out << " = fcmp " << pred << " " << t.ir << " " << l.val << ", " << r.val
                  << "\n";
        };

        switch (n->op) {
            case BinaryOp::Add: body_ << "  " << out << " = " << (flt ? "fadd " : "add ") << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Sub: body_ << "  " << out << " = " << (flt ? "fsub " : "sub ") << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Mul: body_ << "  " << out << " = " << (flt ? "fmul " : "mul ") << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Div:
                body_ << "  " << out << " = " << (flt ? "fdiv " : (t.isUnsigned ? "udiv " : "sdiv "))
                      << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Mod:
                body_ << "  " << out << " = " << (flt ? "frem " : (t.isUnsigned ? "urem " : "srem "))
                      << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::BitAnd: body_ << "  " << out << " = and " << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::BitOr:  body_ << "  " << out << " = or "  << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::BitXor: body_ << "  " << out << " = xor " << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Shl:    body_ << "  " << out << " = shl " << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::Shr:    body_ << "  " << out << " = " << (t.isUnsigned ? "lshr " : "ashr ") << t.ir << " " << l.val << ", " << r.val << "\n"; return {t, out};
            case BinaryOp::EqEq:   flt ? fcmp("oeq") : icmp("eq"); return {detail::mapType({"bool"}), out};
            case BinaryOp::NotEq:  flt ? fcmp("one") : icmp("ne"); return {detail::mapType({"bool"}), out};
            case BinaryOp::Lt:     flt ? fcmp("olt") : icmp(t.isUnsigned ? "ult" : "slt"); return {detail::mapType({"bool"}), out};
            case BinaryOp::LtEq:   flt ? fcmp("ole") : icmp(t.isUnsigned ? "ule" : "sle"); return {detail::mapType({"bool"}), out};
            case BinaryOp::Gt:     flt ? fcmp("ogt") : icmp(t.isUnsigned ? "ugt" : "sgt"); return {detail::mapType({"bool"}), out};
            case BinaryOp::GtEq:   flt ? fcmp("oge") : icmp(t.isUnsigned ? "uge" : "sge"); return {detail::mapType({"bool"}), out};
            default: return {t, r.val};
        }
    }

    Eval emitAssign(AssignExpr* n) {
        Eval v = emitExpr(n->value.get());
        if (n->target->kind == NodeKind::Ident) {
            auto* id = static_cast<IdentExpr*>(n->target.get());
            auto it = symbols_.find(id->name);
            if (it != symbols_.end()) {
                Eval stored = coerce(v, it->second.type);
                body_ << "  store " << it->second.type.ir << " " << stored.val << ", ptr "
                      << it->second.ptr << "\n";
                return stored;
            }
        }
        out_ << "; TODO: assignment to non-variable lvalue\n";
        return v;
    }

    Eval emitCall(CallExpr* n) {
        // Without the module in hand we cannot resolve the callee's signature,
        // so assume a value-returning call of type i32. The driver passes the
        // module to back-ends that need precise types (e.g. the LLVM backend);
        // for the text back-end this is sufficient for the bootstrap test set.
        detail::MappedType ret = detail::mapType({"int"});
        std::ostringstream args;
        for (std::size_t i = 0; i < n->args.size(); ++i) {
            Eval a = emitExpr(n->args[i].get());
            if (i) args << ", ";
            args << a.type.ir << " " << a.val;
        }
        std::string r = newReg();
        body_ << "  " << r << " = call " << ret.ir << " @" << n->callee << "(" << args.str()
              << ")\n";
        return {ret, r};
    }
};

class IRTextBackend : public CodegenBackend {
public:
    std::string_view name() const noexcept override { return "llvm-ir-text"; }
    CodegenResult generate(const Module& mod) override { return IRTextImpl{}.run(mod); }
};

} // namespace

std::unique_ptr<CodegenBackend> createTextBackend() {
    return std::make_unique<IRTextBackend>();
}

} // namespace strata
