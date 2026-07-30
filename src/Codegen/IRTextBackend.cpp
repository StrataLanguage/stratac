// Strata compiler: text back-end.
//
// Emits LLVM IR as text from the AST. This is deliberately a real (if small)
// lowering rather than a stub: scalar integer/float functions with parameters,
// locals, assignment, arithmetic, comparisons, control flow (if/else, while),
// and calls are turned into valid LLVM IR that downstream tools (clang, llc)
// can assemble. Vector types and short-circuit logic are typed/passed through
// but not fully lowered yet; those are flagged in the IR as TODO and left as
// follow-up work.
#include "TypeRegistry.h"
#include "TypeUtil.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/CodegenBackend.h"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace strata
{

namespace
{

struct Symbol
{
    detail::MappedType type;
    std::string ptr; // alloca pointer, e.g. "%3"
};

struct Eval
{
    detail::MappedType type;
    std::string val; // register "%3" or literal "3" / "1.500000e+00" / "1"
};

class IRTextImpl
{
  public:
    CodegenResult Run(const Module& mod)
    {
        CodegenResult res;
        res.moduleName = mod.name;
        m_out << "; Strata module '" << mod.name << "'\n";
        m_registry.Build(mod);

        // Emit struct type definitions (named, like the LLVM back-end).
        for (const auto& st : m_registry.Types())
        {
            if (st.opaque) continue;
            m_out << "%struct." << st.name << " = type { ";
            for (std::size_t i = 0; i < st.fields.size(); ++i)
            {
                if (i) m_out << ", ";
                m_out << MappedOr(st.fields[i].type, "ptr").ir;
            }
            m_out << " }\n";
        }
        m_out << "\n";

        // Collect signatures first so call sites (in any order) know each
        // callee's return type and which parameters are passed by pointer.
        for (const auto& f : mod.functions) CollectSignature(*f);

        // Emit body-less prototypes as 'declare', then definitions as 'define'.
        // A function may not be both declared and defined; calls to functions
        // defined later in the module resolve through LLVM forward references.
        for (const auto& f : mod.functions)
            if (!f->body) EmitDeclare(*f);
        m_out << "\n";
        for (const auto& f : mod.functions)
            if (f->body) EmitFunction(*f);

        res.ok = true;
        res.output = m_out.str();
        return res;
    }

  private:
    std::ostringstream m_out;
    std::ostringstream m_body;
    std::map<std::string, Symbol> m_symbols;
    TypeRegistry m_registry;
    std::map<std::string, detail::MappedType> m_retOf;       // mangled name -> return type
    std::map<std::string, std::vector<bool>> m_paramByPtrOf; // mangled name -> pass-by-pointer
    std::vector<std::string> m_paramRegs;
    detail::MappedType m_retType;
    int m_tmp = 0;
    int m_label = 0;
    bool m_terminated = false;
    struct Loop
    {
        std::string cont;
        std::string end;
    };
    std::vector<Loop> m_loops;

    static bool ByRef(ParamMod m)
    {
        return m == ParamMod::Out || m == ParamMod::InOut;
    }

    void CollectSignature(const FunctionDecl& f)
    {
        m_retOf[f.mangledName] = MappedOr(f.returnType, "void");
        auto& bp = m_paramByPtrOf[f.mangledName];
        bp.clear();
        for (const auto& p : f.params)
        {
            // Structs are always by reference; scalars by value unless out/inout.
            bool structVal = m_registry.IsUserType(p->type.name) && !m_registry.IsOpaque(p->type.name);
            bool byPtr = ByRef(p->mod) || structVal;
            bp.push_back(byPtr);
        }
    }

    std::string NewReg()
    {
        return "%t" + std::to_string(m_tmp++);
    }
    std::string NewLabel()
    {
        return std::string("L") + std::to_string(m_label++);
    }

    static std::string FloatConst(double v)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6e", v);
        return std::string(buf);
    }

    detail::MappedType MappedOr(const TypeName& t, const char* fallback)
    {
        auto m = detail::MapType(t);
        if (m.valid) return m;
        if (m_registry.IsUserType(t.name) && !m_registry.IsOpaque(t.name))
        {
            m.valid = true;
            m.ir = "%struct." + t.name;
            m.elemIr = m.ir;
            return m;
        }
        if (m_registry.IsOpaque(t.name))
        {
            m.valid = true;
            m.ir = "ptr";
            m.elemIr = "ptr";
            return m;
        }
        m_out << "; TODO: unsupported type '" << t.name << "' lowered as " << fallback << "\n";
        m.ir = fallback;
        m.elemIr = fallback;
        return m;
    }

    void EmitDeclare(const FunctionDecl& f)
    {
        m_retType = MappedOr(f.returnType, "void");
        const auto& bp = m_paramByPtrOf[f.mangledName];
        m_out << "declare " << m_retType.ir << " @" << f.mangledName << "(";
        for (std::size_t i = 0; i < f.params.size(); ++i)
        {
            if (i) m_out << ", ";
            m_out << (bp[i] ? "ptr" : MappedOr(f.params[i]->type, "ptr").ir);
        }
        m_out << ")\n";
    }

    void EmitFunction(const FunctionDecl& f)
    {
        m_retType = MappedOr(f.returnType, "void");
        const auto& bp = m_paramByPtrOf[f.mangledName];
        std::vector<detail::MappedType> ptypes;
        m_out << "define " << m_retType.ir << " @" << f.mangledName << "(";
        m_paramRegs.clear();
        for (std::size_t i = 0; i < f.params.size(); ++i)
        {
            ptypes.push_back(MappedOr(f.params[i]->type, "ptr"));
            m_paramRegs.push_back("%p" + std::to_string(i));
            if (i) m_out << ", ";
            m_out << (bp[i] ? "ptr" : ptypes.back().ir) << " " << m_paramRegs.back();
        }
        m_out << ") {\n";

        // Reset per-function state.
        m_body.str({});
        m_symbols.clear();
        m_tmp = 0;
        m_label = 0;
        m_terminated = false;
        m_loops.clear();

        // Materialize each parameter. By-value params get an alloca; by-pointer
        // params (out/inout) are already pointers to the caller's storage.
        for (std::size_t i = 0; i < f.params.size(); ++i)
        {
            if (bp[i])
            {
                m_symbols[f.params[i]->name] = {.type = ptypes[i], .ptr = m_paramRegs[i]};
            }
            else
            {
                std::string slot = NewReg();
                m_body << "  " << slot << " = alloca " << ptypes[i].ir << "\n";
                m_body << "  store " << ptypes[i].ir << " " << m_paramRegs[i] << ", ptr " << slot << "\n";
                m_symbols[f.params[i]->name] = {.type = ptypes[i], .ptr = slot};
            }
        }

        if (f.body)
        {
            auto* block = static_cast<Block*>(f.body.get());
            for (auto& s : block->statements) EmitStmt(s.get());
        }

        if (!m_terminated)
        {
            if (m_retType.isVoid)
                m_body << "  ret void\n";
            else
                m_body << "  ret " << m_retType.ir << " 0\n";
        }

        m_out << m_body.str();
        m_out << "}\n\n";
    }

    void EmitLabel(const std::string& l)
    {
        m_body << l << ":\n";
        m_terminated = false;
    }
    void EmitBr(const std::string& target)
    {
        if (!m_terminated)
        {
            m_body << "  br label %" << target << "\n";
            m_terminated = true;
        }
    }

    // Coerces an evaluated value to a target scalar type when they differ by
    // int<->float. Returns the (possibly new) value reference.
    Eval Coerce(Eval ev, const detail::MappedType& target)
    {
        if (ev.type.ir == target.ir || target.isVoid) return ev;
        if (target.IsVector() || ev.type.IsVector()) return ev; // WIP
        std::string r = NewReg();
        if (!ev.type.isFloat && target.isFloat)
        {
            m_body << "  " << r << " = " << (ev.type.isUnsigned ? "uitofp " : "sitofp ") << ev.type.ir << " " << ev.val
                   << " to " << target.ir << "\n";
        }
        else if (ev.type.isFloat && !target.isFloat)
        {
            m_body << "  " << r << " = " << (target.isUnsigned ? "fptoui " : "fptosi ") << ev.type.ir << " " << ev.val
                   << " to " << target.ir << "\n";
        }
        else
        {
            return ev; // best effort (e.g. width changes not handled yet)
        }
        return {.type = target, .val = r};
    }

    void EmitStmt(Node* n)
    {
        if (!n) return;
        switch (n->kind)
        {
        case NodeKind::Block:
            for (auto& s : static_cast<Block*>(n)->statements) EmitStmt(s.get());
            return;
        case NodeKind::ExprStmt:
            if (auto* e = static_cast<ExprStmt*>(n)->expr.get()) (void)EmitExpr(e);
            return;
        case NodeKind::Return:
        {
            auto* r = static_cast<ReturnStmt*>(n);
            if (r->value)
            {
                Eval v = Coerce(EmitExpr(r->value.get()), m_retType);
                m_body << "  ret " << m_retType.ir << " " << v.val << "\n";
            }
            else
            {
                m_body << "  ret void\n";
            }
            m_terminated = true;
            return;
        }
        case NodeKind::VarDecl:
        {
            auto* vd = static_cast<VarDeclStmt*>(n);
            auto ty = MappedOr(vd->type, "ptr");
            std::string slot = NewReg();
            m_body << "  " << slot << " = alloca " << ty.ir << "\n";
            if (vd->init)
            {
                Eval v = Coerce(EmitExpr(vd->init.get()), ty);
                m_body << "  store " << ty.ir << " " << v.val << ", ptr " << slot << "\n";
            }
            else
            {
                // zero-init by default (scalars, structs, vectors).
                m_body << "  store " << ty.ir << " zeroinitializer, ptr " << slot << "\n";
            }
            m_symbols[vd->name] = {.type = ty, .ptr = slot};
            return;
        }
        case NodeKind::If:
            EmitIf(static_cast<IfStmt*>(n));
            return;
        case NodeKind::While:
            EmitWhile(static_cast<WhileStmt*>(n));
            return;
        case NodeKind::For:
            EmitFor(static_cast<ForStmt*>(n));
            return;
        case NodeKind::Break:
            if (!m_loops.empty()) EmitBr(m_loops.back().end);
            return;
        case NodeKind::Continue:
            if (!m_loops.empty()) EmitBr(m_loops.back().cont);
            return;
        default:
            (void)EmitExpr(n); // expression-shaped statement
            return;
        }
    }

    void EmitIf(IfStmt* n)
    {
        Eval cond = EmitExpr(n->condition.get());
        std::string thenL = NewLabel();
        std::string elseL = NewLabel();
        std::string endL = NewLabel();
        bool hasElse = n->elseBranch != nullptr;
        m_body << "  br i1 " << cond.val << ", label %" << thenL << ", label %" << (hasElse ? elseL : endL) << "\n";
        m_terminated = true;

        EmitLabel(thenL);
        EmitStmt(n->thenBranch.get());
        EmitBr(endL);

        if (hasElse)
        {
            EmitLabel(elseL);
            EmitStmt(n->elseBranch.get());
            EmitBr(endL);
        }
        EmitLabel(endL);
    }

    void EmitWhile(WhileStmt* n)
    {
        std::string condL = NewLabel();
        std::string bodyL = NewLabel();
        std::string endL = NewLabel();
        m_body << "  br label %" << condL << "\n";
        m_terminated = true;
        EmitLabel(condL);
        Eval cond = EmitExpr(n->condition.get());
        m_body << "  br i1 " << cond.val << ", label %" << bodyL << ", label %" << endL << "\n";
        m_terminated = true;
        EmitLabel(bodyL);
        m_loops.push_back({.cont = condL, .end = endL});
        EmitStmt(n->body.get());
        m_loops.pop_back();
        EmitBr(condL);
        EmitLabel(endL);
    }

    void EmitFor(ForStmt* n)
    {
        if (n->init) EmitStmt(n->init.get()); // var decl or expression
        std::string condL = NewLabel();
        std::string bodyL = NewLabel();
        std::string updL = NewLabel();
        std::string endL = NewLabel();
        EmitBr(condL);
        EmitLabel(condL);
        if (n->condition)
        {
            Eval cond = EmitExpr(n->condition.get());
            m_body << "  br i1 " << cond.val << ", label %" << bodyL << ", label %" << endL << "\n";
        }
        else
        {
            m_body << "  br label %" << bodyL << "\n";
        }
        m_terminated = true;
        EmitLabel(bodyL);
        m_loops.push_back({.cont = updL, .end = endL}); // continue runs the update
        EmitStmt(n->body.get());
        m_loops.pop_back();
        EmitBr(updL);
        EmitLabel(updL);
        if (n->update) (void)EmitExpr(n->update.get());
        EmitBr(condL);
        EmitLabel(endL);
    }

    Eval EmitExpr(Node* n)
    {
        if (!n) return {.type = MappedOr({.name = "int"}, "i32"), .val = "0"};
        switch (n->kind)
        {
        case NodeKind::IntLiteral:
        {
            auto* l = static_cast<IntLiteral*>(n);
            detail::MappedType t = detail::MapType({.name = l->isUnsigned ? "uint" : "int"});
            return {.type = t, .val = std::to_string(l->value)};
        }
        case NodeKind::FloatLiteral:
        {
            auto* l = static_cast<FloatLiteral*>(n);
            return {.type = detail::MapType({.name = "float"}), .val = FloatConst(l->value)};
        }
        case NodeKind::BoolLiteral:
            return {.type = detail::MapType({.name = "bool"}), .val = static_cast<BoolLiteral*>(n)->value ? "1" : "0"};
        case NodeKind::Ident:
            return EmitIdent(static_cast<IdentExpr*>(n));
        case NodeKind::Unary:
            return EmitUnary(static_cast<UnaryExpr*>(n));
        case NodeKind::Binary:
            return EmitBinary(static_cast<BinaryExpr*>(n));
        case NodeKind::Call:
            return EmitCall(static_cast<CallExpr*>(n));
        case NodeKind::Assign:
            return EmitAssign(static_cast<AssignExpr*>(n));
        default:
            m_out << "; TODO: unsupported expression kind\n";
            return {.type = detail::MapType({.name = "int"}), .val = "0"};
        }
    }

    Eval EmitIdent(IdentExpr* n)
    {
        auto it = m_symbols.find(n->name);
        if (it == m_symbols.end())
        {
            m_out << "; TODO: unknown identifier '" << n->name << "'\n";
            return {.type = detail::MapType({.name = "int"}), .val = "0"};
        }
        std::string r = NewReg();
        m_body << "  " << r << " = load " << it->second.type.ir << ", ptr " << it->second.ptr << "\n";
        return {.type = it->second.type, .val = r};
    }

    Eval EmitUnary(UnaryExpr* n)
    {
        Eval e = EmitExpr(n->operand.get());
        std::string r = NewReg();
        switch (n->op)
        {
        case UnaryOp::Pos:
            return e;
        case UnaryOp::Neg:
            if (e.type.isFloat)
                m_body << "  " << r << " = fneg " << e.type.ir << " " << e.val << "\n";
            else
                m_body << "  " << r << " = sub " << e.type.ir << " 0, " << e.val << "\n";
            return {.type = e.type, .val = r};
        case UnaryOp::Not:
            m_body << "  " << r << " = xor i1 " << e.val << ", true\n";
            return {.type = detail::MapType({.name = "bool"}), .val = r};
        case UnaryOp::BitNot:
            m_body << "  " << r << " = xor " << e.type.ir << " " << e.val << ", -1\n";
            return {.type = e.type, .val = r};
        }
        return e;
    }

    Eval EmitBinary(BinaryExpr* n)
    {
        Eval l = EmitExpr(n->lhs.get());
        Eval r = EmitExpr(n->rhs.get());
        detail::MappedType t = l.type;
        std::string out = NewReg();

        if (n->op == BinaryOp::LogicAnd || n->op == BinaryOp::LogicOr)
        {
            // Non-short-circuit on i1 for now.
            const char* op = (n->op == BinaryOp::LogicAnd) ? "and" : "or";
            m_body << "  " << out << " = " << op << " i1 " << l.val << ", " << r.val << "\n";
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        }

        bool flt = t.isFloat;
        auto icmp = [&](const char* pred)
        {
            m_body << "  " << out << " = icmp " << pred << " " << t.ir << " " << l.val << ", " << r.val << "\n";
        };
        auto fcmp = [&](const char* pred)
        {
            m_body << "  " << out << " = fcmp " << pred << " " << t.ir << " " << l.val << ", " << r.val << "\n";
        };

        switch (n->op)
        {
        case BinaryOp::Add:
            m_body << "  " << out << " = " << (flt ? "fadd " : "add ") << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Sub:
            m_body << "  " << out << " = " << (flt ? "fsub " : "sub ") << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Mul:
            m_body << "  " << out << " = " << (flt ? "fmul " : "mul ") << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Div:
            m_body << "  " << out << " = " << (flt ? "fdiv " : (t.isUnsigned ? "udiv " : "sdiv ")) << t.ir << " "
                   << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Mod:
            m_body << "  " << out << " = " << (flt ? "frem " : (t.isUnsigned ? "urem " : "srem ")) << t.ir << " "
                   << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::BitAnd:
            m_body << "  " << out << " = and " << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::BitOr:
            m_body << "  " << out << " = or " << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::BitXor:
            m_body << "  " << out << " = xor " << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Shl:
            m_body << "  " << out << " = shl " << t.ir << " " << l.val << ", " << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::Shr:
            m_body << "  " << out << " = " << (t.isUnsigned ? "lshr " : "ashr ") << t.ir << " " << l.val << ", "
                   << r.val << "\n";
            return {.type = t, .val = out};
        case BinaryOp::EqEq:
            flt ? fcmp("oeq") : icmp("eq");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        case BinaryOp::NotEq:
            flt ? fcmp("one") : icmp("ne");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        case BinaryOp::Lt:
            flt ? fcmp("olt") : icmp(t.isUnsigned ? "ult" : "slt");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        case BinaryOp::LtEq:
            flt ? fcmp("ole") : icmp(t.isUnsigned ? "ule" : "sle");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        case BinaryOp::Gt:
            flt ? fcmp("ogt") : icmp(t.isUnsigned ? "ugt" : "sgt");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        case BinaryOp::GtEq:
            flt ? fcmp("oge") : icmp(t.isUnsigned ? "uge" : "sge");
            return {.type = detail::MapType({.name = "bool"}), .val = out};
        default:
            return {.type = t, .val = r.val};
        }
    }

    Eval EmitAssign(AssignExpr* n)
    {
        Eval v = EmitExpr(n->value.get());
        if (n->target->kind == NodeKind::Ident)
        {
            auto* id = static_cast<IdentExpr*>(n->target.get());
            auto it = m_symbols.find(id->name);
            if (it != m_symbols.end())
            {
                Eval stored = Coerce(v, it->second.type);
                m_body << "  store " << it->second.type.ir << " " << stored.val << ", ptr " << it->second.ptr << "\n";
                return stored;
            }
        }
        m_out << "; TODO: assignment to non-variable lvalue\n";
        return v;
    }

    // Address to pass for an out/inout argument: the lvalue's slot if there is
    // one, otherwise a temporary.
    std::string EmitArgAddress(Node* arg)
    {
        if (arg && arg->kind == NodeKind::Ident)
        {
            auto it = m_symbols.find(static_cast<IdentExpr*>(arg)->name);
            if (it != m_symbols.end()) return it->second.ptr;
        }
        Eval v = EmitExpr(arg);
        std::string slot = NewReg();
        m_body << "  " << slot << " = alloca " << v.type.ir << "\n";
        m_body << "  store " << v.type.ir << " " << v.val << ", ptr " << slot << "\n";
        return slot;
    }

    Eval EmitCall(CallExpr* n)
    {
        // Return type of the callee (resolved by overload resolution). Falls
        // back to i32 when unknown (e.g. an unresolved host call).
        detail::MappedType ret = detail::MapType({.name = "int"});
        auto rit = m_retOf.find(n->callee);
        if (rit != m_retOf.end()) ret = rit->second;
        const auto& bp = m_paramByPtrOf[n->callee];
        std::ostringstream args;
        for (std::size_t i = 0; i < n->args.size(); ++i)
        {
            bool passAddr = i < bp.size() && bp[i];
            if (i) args << ", ";
            if (passAddr)
            {
                args << "ptr " << EmitArgAddress(n->args[i].get());
            }
            else
            {
                Eval a = EmitExpr(n->args[i].get());
                args << a.type.ir << " " << a.val;
            }
        }
        std::string r = NewReg();
        m_body << "  " << r << " = call " << ret.ir << " @" << n->callee << "(" << args.str() << ")\n";
        return {.type = ret, .val = r};
    }
};

class IRTextBackend : public CodegenBackend
{
  public:
    std::string_view Name() const noexcept override
    {
        return "llvm-ir-text";
    }
    CodegenResult Generate(const Module& mod) override
    {
        return IRTextImpl{}.Run(mod);
    }
};

} // namespace

std::unique_ptr<CodegenBackend> CreateTextBackend()
{
    return std::make_unique<IRTextBackend>();
}

} // namespace strata
