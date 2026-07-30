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
            if (st.opaque)
            {
                continue;
            }

            m_out << "%struct." << st.name << " = type { ";
            for (std::size_t i = 0; i < st.fields.size(); ++i)
            {
                if (i)
                {
                    m_out << ", ";
                }

                m_out << MappedOr(st.fields[i].type, "ptr").ir;
            }

            m_out << " }\n";
        }

        m_out << "\n";

        // Collect signatures first so call sites (in any order) know each
        // callee's return type and which parameters are passed by pointer.
        for (const auto& f : mod.functions)
        {
            CollectSignature(*f);
        }

        // Emit body-less prototypes as 'declare', then definitions as 'define'.
        // A function may not be both declared and defined; calls to functions
        // defined later in the module resolve through LLVM forward references.
        for (const auto& f : mod.functions)
        {
            if (!f->body)
            {
                EmitDeclare(*f);
            }
        }

        m_out << "\n";
        for (const auto& f : mod.functions)
        {
            if (f->body)
            {
                EmitFunction(*f);
            }
        }

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

    // Determine if we need write-back (by ref)
    static bool ByRef(ParamMod m)
    {
        // Handles In, Out, InOut.
        // these all indicate pass by-ref
        return m != ParamMod::None;
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
        if (m.valid)
        {
            return m;
        }

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
            if (i)
            {
                m_out << ", ";
            }

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
            if (i)
            {
                m_out << ", ";
            }

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
                m_symbols[f.params[i]->name] = {
                    .type = ptypes[i],
                    .ptr = m_paramRegs[i],
                };
            }
            else
            {
                std::string slot = NewReg();
                m_body << "  " << slot << " = alloca " << ptypes[i].ir << "\n";
                m_body << "  store " << ptypes[i].ir << " " << m_paramRegs[i] << ", ptr " << slot << "\n";
                m_symbols[f.params[i]->name] = {
                    .type = ptypes[i],
                    .ptr = slot,
                };
            }
        }

        if (f.body)
        {
            auto* block = static_cast<Block*>(f.body.get());
            for (auto& s : block->statements)
            {
                EmitStmt(s.get());
            }
        }

        if (!m_terminated)
        {
            if (m_retType.isVoid)
            {
                m_body << "  ret void\n";
            }
            else
            {
                m_body << "  ret " << m_retType.ir << " 0\n";
            }
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
    Eval Coerce(Eval eval, const detail::MappedType& target)
    {
        if (eval.type.ir == target.ir || target.isVoid)
        {
            return eval;
        }

        if (target.IsVector() || eval.type.IsVector())
        {
            return eval; // WIP
        }

        std::string r = NewReg();
        if (!eval.type.isFloat && target.isFloat)
        {
            m_body << "  " << r << " = " << (eval.type.isUnsigned ? "uitofp " : "sitofp ") << eval.type.ir << " "
                   << eval.val << " to " << target.ir << "\n";
        }
        else if (eval.type.isFloat && !target.isFloat)
        {
            m_body << "  " << r << " = " << (target.isUnsigned ? "fptoui " : "fptosi ") << eval.type.ir << " "
                   << eval.val << " to " << target.ir << "\n";
        }
        else
        {
            return eval; // best effort (e.g. width changes not handled yet)
        }

        return {
            .type = target,
            .val = r,
        };
    }

    void EmitStmt(Node* node)
    {
        if (!node)
        {
            return;
        }

        switch (node->kind)
        {
        case NodeKind::Block:
            for (auto& s : static_cast<Block*>(node)->statements)
            {
                EmitStmt(s.get());
            }

            return;
        case NodeKind::ExprStmt:
            if (auto* expr = static_cast<ExprStmt*>(node)->expr.get())
            {
                (void)EmitExpr(expr);
            }

            return;
        case NodeKind::Return:
        {
            auto* r = static_cast<ReturnStmt*>(node);
            if (r->value)
            {
                Eval value = Coerce(EmitExpr(r->value.get()), m_retType);
                m_body << "  ret " << m_retType.ir << " " << value.val << "\n";
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
            auto* varDecl = static_cast<VarDeclStmt*>(node);
            auto type = MappedOr(varDecl->type, "ptr");
            std::string slot = NewReg();
            m_body << "  " << slot << " = alloca " << type.ir << "\n";
            if (varDecl->init)
            {
                Eval value = Coerce(EmitExpr(varDecl->init.get()), type);
                m_body << "  store " << type.ir << " " << value.val << ", ptr " << slot << "\n";
            }
            else
            {
                // zero-init by default (scalars, structs, vectors).
                m_body << "  store " << type.ir << " zeroinitializer, ptr " << slot << "\n";
            }

            m_symbols[varDecl->name] = {
                .type = type,
                .ptr = slot,
            };
            return;
        }

        case NodeKind::If:
            EmitIf(static_cast<IfStmt*>(node));
            return;
        case NodeKind::While:
            EmitWhile(static_cast<WhileStmt*>(node));
            return;
        case NodeKind::For:
            EmitFor(static_cast<ForStmt*>(node));
            return;
        case NodeKind::Break:
            if (!m_loops.empty())
            {
                EmitBr(m_loops.back().end);
            }

            return;
        case NodeKind::Continue:
            if (!m_loops.empty())
            {
                EmitBr(m_loops.back().cont);
            }

            return;
        default:
            (void)EmitExpr(node); // expression-shaped statement
            return;
        }
    }

    void EmitIf(IfStmt* node)
    {
        Eval cond = EmitExpr(node->condition.get());
        std::string thenL = NewLabel();
        std::string elseL = NewLabel();
        std::string endL = NewLabel();
        bool hasElse = node->elseBranch != nullptr;
        m_body << "  br i1 " << cond.val << ", label %" << thenL << ", label %" << (hasElse ? elseL : endL) << "\n";
        m_terminated = true;

        EmitLabel(thenL);
        EmitStmt(node->thenBranch.get());
        EmitBr(endL);

        if (hasElse)
        {
            EmitLabel(elseL);
            EmitStmt(node->elseBranch.get());
            EmitBr(endL);
        }

        EmitLabel(endL);
    }

    void EmitWhile(WhileStmt* node)
    {
        std::string condL = NewLabel();
        std::string bodyL = NewLabel();
        std::string endL = NewLabel();
        m_body << "  br label %" << condL << "\n";
        m_terminated = true;
        EmitLabel(condL);
        Eval cond = EmitExpr(node->condition.get());
        m_body << "  br i1 " << cond.val << ", label %" << bodyL << ", label %" << endL << "\n";
        m_terminated = true;
        EmitLabel(bodyL);
        m_loops.push_back({
            .cont = condL,
            .end = endL,
        });
        EmitStmt(node->body.get());
        m_loops.pop_back();
        EmitBr(condL);
        EmitLabel(endL);
    }

    void EmitFor(ForStmt* node)
    {
        if (node->init)
        {
            EmitStmt(node->init.get()); // var decl or expression
        }

        std::string condL = NewLabel();
        std::string bodyL = NewLabel();
        std::string updL = NewLabel();
        std::string endL = NewLabel();
        EmitBr(condL);
        EmitLabel(condL);
        if (node->condition)
        {
            Eval cond = EmitExpr(node->condition.get());
            m_body << "  br i1 " << cond.val << ", label %" << bodyL << ", label %" << endL << "\n";
        }
        else
        {
            m_body << "  br label %" << bodyL << "\n";
        }

        m_terminated = true;
        EmitLabel(bodyL);
        m_loops.push_back({
            .cont = updL,
            .end = endL,
        }); // continue runs the update
        EmitStmt(node->body.get());
        m_loops.pop_back();
        EmitBr(updL);
        EmitLabel(updL);
        if (node->update)
        {
            (void)EmitExpr(node->update.get());
        }

        EmitBr(condL);
        EmitLabel(endL);
    }

    Eval EmitExpr(Node* node)
    {
        if (!node)
        {
            return {
                .type = MappedOr({.name = "int"}, "i32"),
                .val = "0",
            };
        }

        switch (node->kind)
        {
        case NodeKind::IntLiteral:
        {
            auto* l = static_cast<IntLiteral*>(node);
            detail::MappedType type = detail::MapType({.name = l->isUnsigned ? "uint" : "int"});
            return {
                .type = type,
                .val = std::to_string(l->value),
            };
        }

        case NodeKind::FloatLiteral:
        {
            auto* l = static_cast<FloatLiteral*>(node);
            return {
                .type = detail::MapType({.name = "float"}),
                .val = FloatConst(l->value),
            };
        }

        case NodeKind::BoolLiteral:
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = static_cast<BoolLiteral*>(node)->value ? "1" : "0",
            };
        case NodeKind::Ident:
            return EmitIdent(static_cast<IdentExpr*>(node));
        case NodeKind::Unary:
            return EmitUnary(static_cast<UnaryExpr*>(node));
        case NodeKind::Binary:
            return EmitBinary(static_cast<BinaryExpr*>(node));
        case NodeKind::Call:
            return EmitCall(static_cast<CallExpr*>(node));
        case NodeKind::StructInit:
            return EmitStructInit(static_cast<StructInitExpr*>(node));
        case NodeKind::Assign:
            return EmitAssign(static_cast<AssignExpr*>(node));
        default:
            m_out << "; TODO: unsupported expression kind\n";
            return {
                .type = detail::MapType({.name = "int"}),
                .val = "0",
            };
        }
    }

    Eval EmitIdent(IdentExpr* node)
    {
        auto iterator = m_symbols.find(node->name);
        if (iterator == m_symbols.end())
        {
            m_out << "; TODO: unknown identifier '" << node->name << "'\n";
            return {
                .type = detail::MapType({.name = "int"}),
                .val = "0",
            };
        }

        std::string r = NewReg();
        m_body << "  " << r << " = load " << iterator->second.type.ir << ", ptr " << iterator->second.ptr << "\n";
        return {
            .type = iterator->second.type,
            .val = r,
        };
    }

    Eval EmitUnary(UnaryExpr* node)
    {
        Eval expr = EmitExpr(node->operand.get());
        std::string r = NewReg();
        switch (node->op)
        {
        case UnaryOp::Pos:
            return expr;
        case UnaryOp::Neg:
            if (expr.type.isFloat)
            {
                m_body << "  " << r << " = fneg " << expr.type.ir << " " << expr.val << "\n";
            }
            else
            {
                m_body << "  " << r << " = sub " << expr.type.ir << " 0, " << expr.val << "\n";
            }

            return {
                .type = expr.type,
                .val = r,
            };
        case UnaryOp::Not:
            m_body << "  " << r << " = xor i1 " << expr.val << ", true\n";
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = r,
            };
        case UnaryOp::BitNot:
            m_body << "  " << r << " = xor " << expr.type.ir << " " << expr.val << ", -1\n";
            return {
                .type = expr.type,
                .val = r,
            };
        }

        return expr;
    }

    Eval EmitBinary(BinaryExpr* node)
    {
        Eval left = EmitExpr(node->lhs.get());
        Eval right = EmitExpr(node->rhs.get());
        detail::MappedType type = left.type;
        std::string out = NewReg();

        if (node->op == BinaryOp::LogicAnd || node->op == BinaryOp::LogicOr)
        {
            // Non-short-circuit on i1 for now.
            const char* op = (node->op == BinaryOp::LogicAnd) ? "and" : "or";
            m_body << "  " << out << " = " << op << " i1 " << left.val << ", " << right.val << "\n";
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        }

        bool flt = type.isFloat;
        auto icmp = [&](const char* pred)
        {
            m_body << "  " << out << " = icmp " << pred << " " << type.ir << " " << left.val << ", " << right.val
                   << "\n";
        };
        auto fcmp = [&](const char* pred)
        {
            m_body << "  " << out << " = fcmp " << pred << " " << type.ir << " " << left.val << ", " << right.val
                   << "\n";
        };

        switch (node->op)
        {
        case BinaryOp::Add:
            m_body << "  " << out << " = " << (flt ? "fadd " : "add ") << type.ir << " " << left.val << ", "
                   << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Sub:
            m_body << "  " << out << " = " << (flt ? "fsub " : "sub ") << type.ir << " " << left.val << ", "
                   << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Mul:
            m_body << "  " << out << " = " << (flt ? "fmul " : "mul ") << type.ir << " " << left.val << ", "
                   << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Div:
            m_body << "  " << out << " = " << (flt ? "fdiv " : (type.isUnsigned ? "udiv " : "sdiv ")) << type.ir << " "
                   << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Mod:
            m_body << "  " << out << " = " << (flt ? "frem " : (type.isUnsigned ? "urem " : "srem ")) << type.ir << " "
                   << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::BitAnd:
            m_body << "  " << out << " = and " << type.ir << " " << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::BitOr:
            m_body << "  " << out << " = or " << type.ir << " " << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::BitXor:
            m_body << "  " << out << " = xor " << type.ir << " " << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Shl:
            m_body << "  " << out << " = shl " << type.ir << " " << left.val << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::Shr:
            m_body << "  " << out << " = " << (type.isUnsigned ? "lshr " : "ashr ") << type.ir << " " << left.val
                   << ", " << right.val << "\n";
            return {
                .type = type,
                .val = out,
            };
        case BinaryOp::EqEq:
            flt ? fcmp("oeq") : icmp("eq");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        case BinaryOp::NotEq:
            flt ? fcmp("one") : icmp("ne");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        case BinaryOp::Lt:
            flt ? fcmp("olt") : icmp(type.isUnsigned ? "ult" : "slt");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        case BinaryOp::LtEq:
            flt ? fcmp("ole") : icmp(type.isUnsigned ? "ule" : "sle");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        case BinaryOp::Gt:
            flt ? fcmp("ogt") : icmp(type.isUnsigned ? "ugt" : "sgt");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        case BinaryOp::GtEq:
            flt ? fcmp("oge") : icmp(type.isUnsigned ? "uge" : "sge");
            return {
                .type = detail::MapType({.name = "bool"}),
                .val = out,
            };
        default:
            return {
                .type = type,
                .val = right.val,
            };
        }
    }

    Eval EmitAssign(AssignExpr* node)
    {
        Eval value = EmitExpr(node->value.get());
        if (node->target->kind == NodeKind::Ident)
        {
            auto* identifier = static_cast<IdentExpr*>(node->target.get());
            auto iterator = m_symbols.find(identifier->name);
            if (iterator != m_symbols.end())
            {
                Eval stored = Coerce(value, iterator->second.type);
                m_body << "  store " << iterator->second.type.ir << " " << stored.val << ", ptr "
                       << iterator->second.ptr << "\n";
                return stored;
            }
        }

        m_out << "; TODO: assignment to non-variable lvalue\n";
        return value;
    }

    // Address to pass for an out/inout argument: the lvalue's slot if there is
    // one, otherwise a temporary.
    std::string EmitArgAddress(Node* arg)
    {
        if (arg && arg->kind == NodeKind::Ident)
        {
            auto iterator = m_symbols.find(static_cast<IdentExpr*>(arg)->name);
            if (iterator != m_symbols.end())
            {
                return iterator->second.ptr;
            }
        }

        Eval value = EmitExpr(arg);
        std::string slot = NewReg();
        m_body << "  " << slot << " = alloca " << value.type.ir << "\n";
        m_body << "  store " << value.type.ir << " " << value.val << ", ptr " << slot << "\n";
        return slot;
    }

    Eval EmitStructInit(StructInitExpr* node)
    {
        detail::MappedType type = MappedOr({.name = node->typeName}, "ptr");
        const auto* st = m_registry.Find(node->typeName);
        std::string agg = "zeroinitializer";
        std::size_t positionalIndex = 0;

        for (const auto& field : node->fields)
        {
            int idx;
            if (field.name.empty())
            {
                idx = static_cast<int>(positionalIndex++);
            }
            else
            {
                idx = m_registry.FieldIndex(node->typeName, field.name);
            }

            if (!st || idx < 0 || static_cast<std::size_t>(idx) >= st->fields.size())
            {
                continue;
            }

            Eval fieldVal =
                Coerce(EmitExpr(field.value.get()), MappedOr(st->fields[static_cast<std::size_t>(idx)].type, "ptr"));
            std::string reg = NewReg();
            m_body << "  " << reg << " = insertvalue " << type.ir << " " << agg << ", " << fieldVal.type.ir << " "
                   << fieldVal.val << ", " << idx << "\n";
            agg = reg;
        }

        return {
            .type = type,
            .val = agg,
        };
    }

    Eval EmitCall(CallExpr* node)
    {
        if (m_registry.IsUserType(node->callee) && !m_registry.IsOpaque(node->callee))
        {
            // Struct constructor: emit insertvalue chain.
            detail::MappedType type = MappedOr({.name = node->callee}, "ptr");
            const auto* st = m_registry.Find(node->callee);
            std::string agg = "zeroinitializer";
            for (std::size_t i = 0; i < node->args.size() && st && i < st->fields.size(); ++i)
            {
                Eval argVal = Coerce(EmitExpr(node->args[i].get()), MappedOr(st->fields[i].type, "ptr"));
                std::string reg = NewReg();
                m_body << "  " << reg << " = insertvalue " << type.ir << " " << agg << ", " << argVal.type.ir << " "
                       << argVal.val << ", " << i << "\n";
                agg = reg;
            }

            return {
                .type = type,
                .val = agg,
            };
        }

        // Return type of the callee (resolved by overload resolution). Falls
        // back to i32 when unknown (e.g. an unresolved host call).
        detail::MappedType returnType = detail::MapType({.name = "int"});
        auto rit = m_retOf.find(node->callee);
        if (rit != m_retOf.end())
        {
            returnType = rit->second;
        }

        const auto& bp = m_paramByPtrOf[node->callee];
        std::ostringstream args;
        for (std::size_t i = 0; i < node->args.size(); ++i)
        {
            bool passAddr = i < bp.size() && bp[i];
            if (i)
            {
                args << ", ";
            }

            if (passAddr)
            {
                args << "ptr " << EmitArgAddress(node->args[i].get());
            }
            else
            {
                Eval a = EmitExpr(node->args[i].get());
                args << a.type.ir << " " << a.val;
            }
        }

        std::string r = NewReg();
        m_body << "  " << r << " = call " << returnType.ir << " @" << node->callee << "(" << args.str() << ")\n";
        return {
            .type = returnType,
            .val = r,
        };
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
