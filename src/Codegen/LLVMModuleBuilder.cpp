// Strata compiler: LLVM module construction (shared by printer/AOT/JIT).
//
// Translates a Strata AST into a live LLVM module via the C API. Handles the
// bootstrap subset: scalar/vector built-ins, user-defined structs (value types
// with member access and positional construction), opaque handle types
// (`handle Name;`, lowered to `ptr`), and functions with `extern` host bindings.
#include "LLVMModuleBuilder.h"
#include "TypeRegistry.h"
#include "TypeUtil.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace strata
{

namespace
{

using namespace strata::llvm_c;

LLVMTypeRef ScalarLlvmType(LLVMContextRef ctx, const detail::MappedType& t)
{
    if (t.isVoid)
    {
        return LLVMVoidTypeInContext(ctx);
    }

    LLVMTypeRef elem = nullptr;
    if (t.elemIr == "i1")
    {
        elem = LLVMInt1TypeInContext(ctx);
    }
    else if (t.elemIr == "i32")
    {
        elem = LLVMInt32TypeInContext(ctx);
    }
    else if (t.elemIr == "half")
    {
        elem = LLVMHalfTypeInContext(ctx);
    }
    else if (t.elemIr == "float")
    {
        elem = LLVMFloatTypeInContext(ctx);
    }
    else if (t.elemIr == "double")
    {
        elem = LLVMDoubleTypeInContext(ctx);
    }
    else
    {
        elem = LLVMInt32TypeInContext(ctx);
    }

    if (t.IsVector())
    {
        return LLVMVectorType(elem, static_cast<unsigned>(t.vec));
    }

    return elem;
}

struct TypeDesc
{
    LLVMTypeRef ty = nullptr;
    bool isFloat = false;
    bool isUnsigned = false;
    bool isVoid = false;
    std::string structName; // non-empty when this is a user struct value
};

struct Value
{
    LLVMValueRef v = nullptr;
    TypeDesc td;
};

struct FuncInfo
{
    LLVMValueRef fn = nullptr;
    LLVMTypeRef ty = nullptr;
    TypeDesc ret;
    std::vector<bool> paramByPtr; // params passed by address (out/inout, or any struct)
};

struct LValue
{
    bool ok = false;
    LLVMValueRef ptr = nullptr;
    TypeDesc td;
};

class Builder
{
  public:
    BuiltModule Build(const Module& module, std::string& notes, bool jitMode)
    {
        notes = "; LLVM C API back-end\n";

        m_jitMode = jitMode;
        m_ctx = LLVMContextCreate();
        m_mod = LLVMModuleCreateWithNameInContext(module.name.c_str(), m_ctx);
        m_builder = LLVMCreateBuilderInContext(m_ctx);
        m_ptrTy = LLVMPointerTypeInContext(m_ctx, 0);

        m_registry.Build(module);

        // Create all struct type handles first (allows mutual references),
        // then fill in bodies.
        for (const auto& st : m_registry.Types())
        {
            if (st.opaque)
            {
                m_structTypes[st.name] = m_ptrTy; // opaque handles are pointer-sized
            }
            else
            {
                m_structTypes[st.name] = LLVMStructCreateNamed(m_ctx, ("struct." + st.name).c_str());
            }
        }

        for (const auto& st : m_registry.Types())
        {
            if (st.opaque)
            {
                continue;
            }

            std::vector<LLVMTypeRef> fields;
            for (const auto& f : st.fields)
            {
                fields.push_back(Resolve(f.type).ty);
            }

            LLVMStructSetBody(m_structTypes[st.name], fields.data(), static_cast<unsigned>(fields.size()), 0);
        }

        for (const auto& f : module.functions)
        {
            DeclareFunction(*f);
        }

        std::ostringstream bodyNotes;
        m_notesSink = &bodyNotes;

        for (const auto& f : module.functions)
        {
            DefineFunction(*f);
        }

        m_notesSink = nullptr;

        for (const auto& f : module.functions)
        {
            if (f->isExtern)
            {
                m_externNames.push_back(f->name);
            }
        }

        notes += bodyNotes.str();

        if (m_builder)
        {
            LLVMDisposeBuilder(m_builder);
            m_builder = nullptr;
        }

        BuiltModule out(m_ctx, m_mod);
        out.externSymbols = std::move(m_externNames);
        
        m_ctx = nullptr;
        m_mod = nullptr;

        return out;
    }

  private:
    LLVMContextRef m_ctx = nullptr;
    LLVMModuleRef m_mod = nullptr;
    LLVMBuilderRef m_builder = nullptr;
    LLVMTypeRef m_ptrTy = nullptr;
    
    TypeRegistry m_registry;

    std::map<std::string, LLVMTypeRef> m_structTypes;
    std::map<std::string, FuncInfo> m_funcs;
    std::map<std::string, Value> m_symbols; // v = alloca slot
    std::map<std::string, LLVMValueRef> m_externSlots;

    std::vector<std::string> m_externNames;
    
    TypeDesc m_curRet;
    
    bool m_terminated = false;
    bool m_jitMode = false;
    
    std::ostringstream* m_notesSink = nullptr;
    
    LLVMValueRef m_curFn = nullptr;
    
    struct Loop
    {
        LLVMBasicBlockRef cont;
        LLVMBasicBlockRef end;
    };
    
    std::vector<Loop> m_loops;

    LLVMTypeRef I32Ty() const
    {
        return LLVMInt32TypeInContext(m_ctx);
    }

    LLVMTypeRef I1Ty() const
    {
        return LLVMInt1TypeInContext(m_ctx);
    }

    LLVMBasicBlockRef NewBb(const char* name)
    {
        return LLVMAppendBasicBlockInContext(m_ctx, m_curFn, name);
    }

    void PositionAtEnd(LLVMBasicBlockRef bb)
    {
        LLVMPositionBuilderAtEnd(m_builder, bb);
        m_terminated = false;
    }

    void Br(LLVMBasicBlockRef dest)
    {
        if (!m_terminated)
        {
            LLVMBuildBr(m_builder, dest);
            m_terminated = true;
        }
    }

    LLVMValueRef IdxConst(unsigned i) const
    {
        return LLVMConstInt(I32Ty(), i, 1);
    }

    void Note(const std::string& s)
    {
        if (m_notesSink)
        {
            *m_notesSink << s;
        }
    }

    TypeDesc Resolve(const TypeName& t)
    {
        auto m = detail::MapType(t);
        if (m.valid)
        {
            return {.ty = ScalarLlvmType(m_ctx, m),
                    .isFloat = m.isFloat,
                    .isUnsigned = m.isUnsigned,
                    .isVoid = m.isVoid,
                    .structName = ""};
        }

        auto it = m_structTypes.find(t.name);

        if (it != m_structTypes.end())
        {
            return {.ty = it->second, .isFloat = false, .isUnsigned = false, .isVoid = false, .structName = t.name};
        }

        Note("; TODO: unknown type '" + t.name + "' lowered as ptr\n");

        return {.ty = m_ptrTy, .isFloat = false, .isUnsigned = false, .isVoid = false, .structName = ""};
    }

    static LLVMValueRef ZeroOf(const TypeDesc& td)
    {
        return LLVMConstNull(td.ty);
    }

    Value ZeroInt() const
    {
        TypeDesc td{.ty = I32Ty(), .isFloat = false, .isUnsigned = false, .isVoid = false, .structName = ""};
        return {.v = LLVMConstNull(I32Ty()), .td = td};
    }

    // int<->float coercion for scalars.
    Value Coerce(Value v, const TypeDesc& target)
    {
        if (!v.td.ty || !target.ty || v.td.ty == target.ty)
        {
            return v;
        }

        if (!v.td.structName.empty() || !target.structName.empty())
        {
            return v;
        }

        LLVMValueRef r = nullptr;
        if (!v.td.isFloat && target.isFloat)
        {
            r = v.td.isUnsigned ? LLVMBuildUIToFP(m_builder, v.v, target.ty, "c")
                                : LLVMBuildSIToFP(m_builder, v.v, target.ty, "c");
        }
        else if (v.td.isFloat && !target.isFloat)
        {
            r = target.isUnsigned ? LLVMBuildFPToUI(m_builder, v.v, target.ty, "c")
                                  : LLVMBuildFPToSI(m_builder, v.v, target.ty, "c");
        }
        else if (!v.td.isFloat && !target.isFloat && !target.isVoid)
        {
            r = LLVMBuildIntCast2(m_builder, v.v, target.ty, !target.isUnsigned, "c");
        }
        else
        {
            return v;
        }

        return {.v = r, .td = target};
    }

    void DeclareFunction(const FunctionDecl& f)
    {
        FuncInfo info;
        info.ret = Resolve(f.returnType);
        
        std::vector<LLVMTypeRef> params;

        for (const auto& p : f.params)
        {
            // Structs are always passed by reference (out/inout for write-back,
            // and any struct param on any function). Scalars are by value unless
            // out/inout.
            bool structVal = m_registry.IsUserType(p->type.name) && !m_registry.IsOpaque(p->type.name);
            bool byPtr = (p->mod == ParamMod::Out || p->mod == ParamMod::InOut) || structVal;
            info.paramByPtr.push_back(byPtr);
            params.push_back(byPtr ? m_ptrTy : Resolve(p->type).ty);
        }

        info.ty = LLVMFunctionType(info.ret.ty, params.data(), static_cast<unsigned>(params.size()), 0);

        if (m_jitMode && f.isExtern)
        {
            // Externs are not overloadable, so mangledName == name; the host
            // binds the slot by source name.
            LLVMValueRef slot = LLVMAddGlobal(m_mod, m_ptrTy, ("__strata_ext_" + f.name).c_str());
            LLVMSetInitializer(slot, LLVMConstNull(m_ptrTy));
            m_externSlots[f.name] = slot;
            info.fn = nullptr;
        }
        else
        {
            info.fn = LLVMAddFunction(m_mod, f.mangledName.c_str(), info.ty);
        }

        m_funcs[f.mangledName] = info;
    }

    void DefineFunction(const FunctionDecl& f)
    {
        if (!f.body)
        {
            return; // declaration: extern or forward decl
        }

        m_symbols.clear();
        m_terminated = false;
        m_curRet = Resolve(f.returnType);
        m_loops.clear();

        m_curFn = m_funcs[f.mangledName].fn;

        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(m_ctx, m_curFn, "entry");
        LLVMPositionBuilderAtEnd(m_builder, entry);

        for (unsigned i = 0; i < f.params.size(); ++i)
        {
            ParamMod mod = f.params[i]->mod;
            TypeDesc td = Resolve(f.params[i]->type);
            bool structVal = m_registry.IsUserType(f.params[i]->type.name) && !m_registry.IsOpaque(f.params[i]->type.name);

            // out/inout (write-back) and all struct params arrive as pointers to
            // the caller's storage; reads/writes go through them directly.
            if (mod == ParamMod::Out || mod == ParamMod::InOut || structVal)
            {
                m_symbols[f.params[i]->name] = {.v = LLVMGetParam(m_curFn, i), .td = td};
            }
            else
            {
                LLVMValueRef slot = LLVMBuildAlloca(m_builder, td.ty, "arg");
                LLVMBuildStore(m_builder, LLVMGetParam(m_curFn, i), slot);

                m_symbols[f.params[i]->name] = {.v = slot, .td = td};
            }
        }

        for (const auto& s : static_cast<Block*>(f.body.get())->statements)
        {
            EmitStmt(s.get());

            if (m_terminated)
            {
                break;
            }
        }

        if (!m_terminated)
        {
            if (m_curRet.isVoid)
            {
                LLVMBuildRetVoid(m_builder);
            }
            else
            {
                LLVMBuildRet(m_builder, ZeroOf(m_curRet));
            }
        }
    }

    void EmitStmt(Node* n)
    {
        if (!n)
        {
            return;
        }

        switch (n->kind)
        {
        case NodeKind::Return:
        {
            auto* r = static_cast<ReturnStmt*>(n);

            if (r->value)
            {
                Value v = Coerce(EmitExpr(r->value.get()), m_curRet);
                LLVMBuildRet(m_builder, v.v);
            }
            else
            {
                LLVMBuildRetVoid(m_builder);
            }

            m_terminated = true;
            return;
        }

        case NodeKind::ExprStmt:
            if (auto* e = static_cast<ExprStmt*>(n)->expr.get())
            {
                (void)EmitExpr(e);
            }

            return;
        case NodeKind::VarDecl:
        {
            auto* vd = static_cast<VarDeclStmt*>(n);
            
            TypeDesc td = Resolve(vd->type);
            
            LLVMValueRef slot = LLVMBuildAlloca(m_builder, td.ty, "v");

            if (vd->init)
            {
                Value v = Coerce(EmitExpr(vd->init.get()), td);
                LLVMBuildStore(m_builder, v.v, slot);
            }
            else
            {
                LLVMBuildStore(m_builder, ZeroOf(td), slot);
            }

            m_symbols[vd->name] = {.v = slot, .td = td};

            return;
        }

        case NodeKind::Block:
            for (auto& s : static_cast<Block*>(n)->statements)
            {
                EmitStmt(s.get());
            }

            return;
        case NodeKind::If:
        {
            auto* i = static_cast<IfStmt*>(n);
            
            LLVMValueRef cond = ToI1(EmitExpr(i->condition.get()));
            
            LLVMBasicBlockRef thenBB = NewBb("if.then");
            LLVMBasicBlockRef endBB = NewBb("if.end");
            LLVMBasicBlockRef elseBB = i->elseBranch ? NewBb("if.else") : endBB;

            LLVMBuildCondBr(m_builder, cond, thenBB, elseBB);

            m_terminated = true;
            
            PositionAtEnd(thenBB);
            
            EmitStmt(i->thenBranch.get());

            Br(endBB);
            
            if (i->elseBranch)
            {
                PositionAtEnd(elseBB);
                
                EmitStmt(i->elseBranch.get());

                Br(endBB);
            }

            PositionAtEnd(endBB);

            return;
        }

        case NodeKind::While:
        {
            auto* w = static_cast<WhileStmt*>(n);
            
            LLVMBasicBlockRef condBB = NewBb("while.cond");
            LLVMBasicBlockRef bodyBB = NewBb("while.body");
            LLVMBasicBlockRef endBB = NewBb("while.end");

            LLVMBuildBr(m_builder, condBB);
            
            m_terminated = true;
            
            PositionAtEnd(condBB);
            
            LLVMValueRef cond = ToI1(EmitExpr(w->condition.get()));
            
            LLVMBuildCondBr(m_builder, cond, bodyBB, endBB);
            
            m_terminated = true;
            
            PositionAtEnd(bodyBB);
            
            m_loops.push_back({.cont = condBB, .end = endBB});
            
            EmitStmt(w->body.get());
            
            m_loops.pop_back();
            
            Br(condBB);
            
            PositionAtEnd(endBB);
            
            return;
        }

        case NodeKind::For:
        {
            auto* fs = static_cast<ForStmt*>(n);
            if (fs->init)
            {
                EmitStmt(fs->init.get()); // var decl or expression
            }

            LLVMBasicBlockRef condBB = NewBb("for.cond");
            LLVMBasicBlockRef bodyBB = NewBb("for.body");
            LLVMBasicBlockRef updBB = NewBb("for.update");
            LLVMBasicBlockRef endBB = NewBb("for.end");

            
            Br(condBB);
            
            PositionAtEnd(condBB);
            
            if (fs->condition)
            {
                LLVMBuildCondBr(m_builder, ToI1(EmitExpr(fs->condition.get())), bodyBB, endBB);
            }
            else
            {
                LLVMBuildBr(m_builder, bodyBB);
            }

            m_terminated = true;
            
            PositionAtEnd(bodyBB);

            m_loops.push_back({.cont = updBB, .end = endBB}); // continue runs the update
            
            EmitStmt(fs->body.get());
            
            m_loops.pop_back();
            
            Br(updBB);
            
            PositionAtEnd(updBB);
            
            if (fs->update)
            {
                (void)EmitExpr(fs->update.get());
            }

            Br(condBB);

            PositionAtEnd(endBB);

            return;
        }

        case NodeKind::Break:
            if (!m_loops.empty())
            {
                Br(m_loops.back().end);
            }

            return;
        case NodeKind::Continue:
            if (!m_loops.empty())
            {
                Br(m_loops.back().cont);
            }

            return;
        default:
            (void)EmitExpr(n); // expression-shaped statement
            return;
        }
    }

    // Coerce a value to i1 for use as a branch condition.
    LLVMValueRef ToI1(const Value& v)
    {
        if (v.td.ty == I1Ty())
        {
            return v.v;
        }

        if (v.td.isFloat)
        {
            return LLVMBuildFCmp(m_builder, "one", v.v, LLVMConstNull(v.td.ty), "tobool");
        }

        return LLVMBuildICmp(m_builder, v.td.isUnsigned ? LLVMIntNE : LLVMIntNE, v.v, LLVMConstNull(v.td.ty), "tobool");
    }

    Value EmitExpr(Node* n)
    {
        if (!n)
        {
            return ZeroInt();
        }

        switch (n->kind)
        {
        case NodeKind::IntLiteral:
        {
            auto* l = static_cast<IntLiteral*>(n);

            TypeDesc td{
                .ty = I32Ty(),
                .isFloat = false,
                .isUnsigned = l->isUnsigned,
                .isVoid = false,
                .structName = ""
            };

            return {.v = LLVMConstInt(I32Ty(), l->value, 1), .td = td};
        }

        case NodeKind::FloatLiteral:
        {
            auto* l = static_cast<FloatLiteral*>(n);
            TypeDesc td{
                .ty = LLVMFloatTypeInContext(m_ctx),
                .isFloat = true,
                .isUnsigned = false,
                .isVoid = false,
                .structName = ""
            };

            return {.v = LLVMConstReal(LLVMFloatTypeInContext(m_ctx), l->value), .td = td};
        }

        case NodeKind::BoolLiteral:
            return {
                .v = LLVMConstInt(I1Ty(), static_cast<unsigned long long>(static_cast<BoolLiteral*>(n)->value), 0),
                .td = {
                    .ty = I1Ty(),
                    .isFloat = false,
                    .isUnsigned = false,
                    .isVoid = false,
                    .structName = ""
                }
            };
        case NodeKind::Ident:
            return EmitIdent(static_cast<IdentExpr*>(n));
        case NodeKind::Unary:
            return EmitUnary(static_cast<UnaryExpr*>(n));
        case NodeKind::Binary:
            return EmitBinary(static_cast<BinaryExpr*>(n));
        case NodeKind::Assign:
            return EmitAssign(static_cast<AssignExpr*>(n));
        case NodeKind::Call:
            return EmitCall(static_cast<CallExpr*>(n));
        case NodeKind::Member:
            return EmitMember(static_cast<MemberExpr*>(n));
        default:
            Note("; TODO: LLVM back-end does not lower this expression yet\n");
            return ZeroInt();
        }
    }

    Value EmitIdent(IdentExpr* n)
    {
        auto it = m_symbols.find(n->name);

        if (it == m_symbols.end())
        {
            Note("; TODO: unknown identifier '" + n->name + "'\n");

            return ZeroInt();
        }

        LLVMValueRef v = LLVMBuildLoad2(m_builder, it->second.td.ty, it->second.v, "id");

        return {.v = v, .td = it->second.td};
    }

    // Resolves an lvalue (a variable or a member-access chain rooted at one) to
    // its storage pointer and element type.
    LValue EmitLValue(Node* n)
    {
        LValue none;

        if (!n)
        {
            return none;
        }

        if (n->kind == NodeKind::Ident)
        {
            auto* id = static_cast<IdentExpr*>(n);

            auto it = m_symbols.find(id->name);

            if (it == m_symbols.end())
            {
                return none;
            }

            return {
                .ok = true,
                .ptr = it->second.v,
                .td = it->second.td
            };
        }

        if (n->kind == NodeKind::Member)
        {
            auto* m = static_cast<MemberExpr*>(n);
            LValue base = EmitLValue(m->base.get());
            if (!base.ok || base.td.structName.empty())
            {
                return none;
            }

            int idx = m_registry.FieldIndex(base.td.structName, m->member);
            if (idx < 0)
            {
                return none;
            }

            const auto* st = m_registry.Find(base.td.structName);
            
            TypeDesc fieldTd = Resolve(st->fields[static_cast<std::size_t>(idx)].type);
            
            LLVMValueRef idxs[2] = {IdxConst(0), IdxConst(static_cast<unsigned>(idx))};
            LLVMValueRef ptr = LLVMBuildGEP2(m_builder, base.td.ty, base.ptr, idxs, 2, "f");

            return {
                .ok = true,
                .ptr = ptr,
                .td = fieldTd
            };
        }

        return none;
    }

    Value EmitMember(MemberExpr* n)
    {
        LValue lv = EmitLValue(n);
        
        if (lv.ok)
        {
            LLVMValueRef v = LLVMBuildLoad2(m_builder, lv.td.ty, lv.ptr, "m");

            return {
                .v = v,
                .td = lv.td
            };
        }

        // Rvalue member access on a struct value (e.g. getVec().x).
        Value base = EmitExpr(n->base.get());

        if (!base.td.structName.empty())
        {
            int idx = m_registry.FieldIndex(base.td.structName, n->member);

            if (idx >= 0)
            {
                const auto* st = m_registry.Find(base.td.structName);
                
                TypeDesc fieldTd = Resolve(st->fields[static_cast<std::size_t>(idx)].type);
                
                LLVMValueRef v = LLVMBuildExtractValue(m_builder, base.v, static_cast<unsigned>(idx), "m");

                return {
                    .v = v,
                    .td = fieldTd
                };
            }
        }

        Note("; TODO: cannot access member '" + n->member + "'\n");

        return ZeroInt();
    }

    Value EmitUnary(UnaryExpr* n)
    {
        Value e = EmitExpr(n->operand.get());
        switch (n->op)
        {
        case UnaryOp::Pos:
            return e;
        case UnaryOp::Neg:
        {
            LLVMValueRef r = e.td.isFloat ? LLVMBuildFNeg(m_builder, e.v, "neg") : LLVMBuildNeg(m_builder, e.v, "neg");

            return {
                .v = r,
                .td = e.td
            };
        }

        case UnaryOp::Not:
        {
            LLVMValueRef r = LLVMBuildXor(m_builder, e.v, LLVMConstInt(I1Ty(), 1, 0), "not");

            return {
                .v = r,
                .td = {
                    .ty = I1Ty(),
                    .isFloat = false,
                    .isUnsigned = false,
                    .isVoid = false,
                    .structName = ""
                }
            };
        }

        case UnaryOp::BitNot:
        {
            LLVMValueRef r = LLVMBuildNot(m_builder, e.v, "bnot");

            return {
                .v = r,
                .td = e.td
            };
        }
        }

        return e;
    }

    Value EmitBinary(BinaryExpr* n)
    {
        Value l = EmitExpr(n->lhs.get());
        Value r = EmitExpr(n->rhs.get());

        // Promote mixed int/float operands to float so arithmetic is well-typed.
        TypeDesc td = l.td;

        if (l.td.isFloat && !r.td.isFloat)
        {
            r = Coerce(r, l.td);
        }
        else if (!l.td.isFloat && r.td.isFloat)
        {
            l = Coerce(l, r.td);
            td = r.td;
        }

        LLVMValueRef out = nullptr;
        bool flt = td.isFloat;

        if (n->op == BinaryOp::LogicAnd || n->op == BinaryOp::LogicOr)
        {
            const char* op = (n->op == BinaryOp::LogicAnd) ? "and" : "or";

            // non-short-circuit on i1
            out = (n->op == BinaryOp::LogicAnd) ? LLVMBuildAnd(m_builder, l.v, r.v, "and")
                                                : LLVMBuildOr(m_builder, l.v, r.v, "or");
            (void)op;

            return {
                .v = out,
                .td = {
                    .ty = I1Ty(),
                    .isFloat = false,
                    .isUnsigned = false,
                    .isVoid = false,
                    .structName = ""
                }
            };
        }

        auto icmp = [&](const char* pred)
        {
            out = LLVMBuildICmp(m_builder, PredNameToPredicate(pred, td.isUnsigned), l.v, r.v, "cmp");
        };

        auto fcmp = [&](const char* pred)
        {
            out = LLVMBuildFCmp(m_builder, pred, l.v, r.v, "cmp");
        };

        TypeDesc boolTd{.ty = I1Ty(), .isFloat = false, .isUnsigned = false, .isVoid = false, .structName = ""};

        switch (n->op)
        {
        case BinaryOp::Add:
            out = flt ? LLVMBuildFAdd(m_builder, l.v, r.v, "add") : LLVMBuildAdd(m_builder, l.v, r.v, "add");
            return {.v = out, .td = td};
        case BinaryOp::Sub:
            out = flt ? LLVMBuildFSub(m_builder, l.v, r.v, "sub") : LLVMBuildSub(m_builder, l.v, r.v, "sub");
            return {.v = out, .td = td};
        case BinaryOp::Mul:
            out = flt ? LLVMBuildFMul(m_builder, l.v, r.v, "mul") : LLVMBuildMul(m_builder, l.v, r.v, "mul");
            return {.v = out, .td = td};
        case BinaryOp::Div:
            out = flt ? LLVMBuildFDiv(m_builder, l.v, r.v, "div")
                      : (td.isUnsigned ? LLVMBuildUDiv(m_builder, l.v, r.v, "div")
                                       : LLVMBuildSDiv(m_builder, l.v, r.v, "div"));
            return {.v = out, .td = td};
        case BinaryOp::Mod:
            out = flt ? LLVMBuildFRem(m_builder, l.v, r.v, "mod")
                      : (td.isUnsigned ? LLVMBuildURem(m_builder, l.v, r.v, "mod")
                                       : LLVMBuildSRem(m_builder, l.v, r.v, "mod"));
            return {.v = out, .td = td};
        case BinaryOp::BitAnd:
            out = LLVMBuildAnd(m_builder, l.v, r.v, "and");
            return {.v = out, .td = td};
        case BinaryOp::BitOr:
            out = LLVMBuildOr(m_builder, l.v, r.v, "or");
            return {.v = out, .td = td};
        case BinaryOp::BitXor:
            out = LLVMBuildXor(m_builder, l.v, r.v, "xor");
            return {.v = out, .td = td};
        case BinaryOp::Shl:
            out = LLVMBuildShl(m_builder, l.v, r.v, "shl");
            return {.v = out, .td = td};
        case BinaryOp::Shr:
            out = td.isUnsigned ? LLVMBuildLShr(m_builder, l.v, r.v, "shr") : LLVMBuildAShr(m_builder, l.v, r.v, "shr");
            return {.v = out, .td = td};
        case BinaryOp::EqEq:
            flt ? fcmp("oeq") : icmp("eq");
            return {.v = out, .td = boolTd};
        case BinaryOp::NotEq:
            flt ? fcmp("one") : icmp("ne");
            return {.v = out, .td = boolTd};
        case BinaryOp::Lt:
            flt ? fcmp("olt") : icmp("lt");
            return {.v = out, .td = boolTd};
        case BinaryOp::LtEq:
            flt ? fcmp("ole") : icmp("le");
            return {.v = out, .td = boolTd};
        case BinaryOp::Gt:
            flt ? fcmp("ogt") : icmp("gt");
            return {.v = out, .td = boolTd};
        case BinaryOp::GtEq:
            flt ? fcmp("oge") : icmp("ge");
            return {.v = out, .td = boolTd};
        default:
            return l;
        }
    }

    static LLVMIntPredicate PredNameToPredicate(const char* p, bool uns)
    {
        if (std::strcmp(p, "eq") == 0)
        {
            return uns ? LLVMIntEQ : LLVMIntEQ;
        }

        if (std::strcmp(p, "ne") == 0)
        {
            return uns ? LLVMIntNE : LLVMIntNE;
        }

        if (std::strcmp(p, "lt") == 0)
        {
            return uns ? LLVMIntULT : LLVMIntSLT;
        }

        if (std::strcmp(p, "le") == 0)
        {
            return uns ? LLVMIntULE : LLVMIntSLE;
        }

        if (std::strcmp(p, "gt") == 0)
        {
            return uns ? LLVMIntUGT : LLVMIntSGT;
        }

        if (std::strcmp(p, "ge") == 0)
        {
            return uns ? LLVMIntUGE : LLVMIntSGE;
        }

        return LLVMIntEQ;
    }

    Value EmitAssign(AssignExpr* n)
    {
        Value v = EmitExpr(n->value.get());
        if (n->target->kind == NodeKind::Ident || n->target->kind == NodeKind::Member)
        {
            LValue lv = EmitLValue(n->target.get());
            if (lv.ok)
            {
                Value stored = Coerce(v, lv.td);
                LLVMBuildStore(m_builder, stored.v, lv.ptr);
                return stored;
            }
        }

        Note("; TODO: assignment to unsupported lvalue\n");
        return v;
    }

    Value EmitCall(CallExpr* n)
    {
        // Constructor: callee names a struct type.
        if (m_structTypes.contains(n->callee))
        {
            TypeDesc td = Resolve({.name = n->callee});
            const auto* st = m_registry.Find(n->callee);
            LLVMValueRef agg = LLVMGetUndef(td.ty);
            for (std::size_t i = 0; i < n->args.size() && st; ++i)
            {
                if (i >= st->fields.size())
                {
                    break;
                }

                Value av = Coerce(EmitExpr(n->args[i].get()), Resolve(st->fields[i].type));
                agg = LLVMBuildInsertValue(m_builder, agg, av.v, static_cast<unsigned>(i), "ins");
            }

            return {.v = agg, .td = td};
        }

        auto it = m_funcs.find(n->callee);
        if (it == m_funcs.end())
        {
            Note("; TODO: call to unknown function '" + n->callee + "'\n");
            return ZeroInt();
        }

        const auto& byPtr = it->second.paramByPtr;
        std::vector<LLVMValueRef> args;
        for (std::size_t k = 0; k < n->args.size(); ++k)
        {
            bool passAddr = k < byPtr.size() && byPtr[k];
            args.push_back(passAddr ? ArgAddress(n->args[k].get()) : EmitExpr(n->args[k].get()).v);
        }

        LLVMValueRef callee = it->second.fn;
        if (m_jitMode)
        {
            auto sit = m_externSlots.find(n->callee);
            if (sit != m_externSlots.end())
            {
                LLVMValueRef fnPtr = LLVMBuildLoad2(m_builder, m_ptrTy, sit->second, "extfn");
                callee = fnPtr;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(m_builder, it->second.ty, callee, args.data(), static_cast<unsigned>(args.size()), "call");

        return {.v = call, .td = it->second.ret};
    }

    // Address to pass for an out/inout argument: the lvalue's storage if there
    // is one, otherwise a temporary (writes discarded).
    LLVMValueRef ArgAddress(Node* arg)
    {
        LValue lv = EmitLValue(arg);
        if (lv.ok)
        {
            return lv.ptr;
        }

        Value v = EmitExpr(arg);
        LLVMValueRef slot = LLVMBuildAlloca(m_builder, v.td.ty, "outarg");
        LLVMBuildStore(m_builder, v.v, slot);
        return slot;
    }
};

} // namespace

BuiltModule BuildLlvmModule(const Module& ast, std::string& notes, bool jitMode)
{
    Builder b;
    return b.Build(ast, notes, jitMode);
}

} // namespace strata
