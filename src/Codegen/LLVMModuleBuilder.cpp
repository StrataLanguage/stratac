#include "LLVMModuleBuilder.h"
#include "TypeRegistry.h"
#include "TypeUtil.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"

#include <cstring>
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
    LLVMTypeRef type = nullptr;
    bool isFloat = false;
    bool isUnsigned = false;
    bool isVoid = false;
    std::string structTypeName;
};

struct Value
{
    LLVMValueRef value = nullptr;
    TypeDesc typeDesc;
};

struct FuncInfo
{
    LLVMValueRef function = nullptr;
    LLVMTypeRef type = nullptr;
    TypeDesc returnType;
    std::vector<bool> paramByPtr;
};

struct LValue
{
    bool valid = false;
    LLVMValueRef ptr = nullptr;
    TypeDesc typeDesc;
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
        for (const auto& structType : m_registry.Types())
        {
            if (structType.opaque)
            {
                m_structTypes[structType.name] = m_ptrTy;
            }
            else
            {
                m_structTypes[structType.name] = LLVMStructCreateNamed(m_ctx, ("struct." + structType.name).c_str());
            }
        }

        for (const auto& structType : m_registry.Types())
        {
            if (structType.opaque)
            {
                continue;
            }

            std::vector<LLVMTypeRef> fields;
            for (const auto& f : structType.fields)
            {
                fields.push_back(Resolve(f.type).type);
            }

            LLVMStructSetBody(m_structTypes[structType.name], fields.data(), static_cast<unsigned>(fields.size()), 0);
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
        auto mapped = detail::MapType(t);
        if (mapped.valid)
        {
            return {
                .type = ScalarLlvmType(m_ctx, mapped),
                .isFloat = mapped.isFloat,
                .isUnsigned = mapped.isUnsigned,
                .isVoid = mapped.isVoid,
                .structTypeName = "",
            };
        }

        auto iterator = m_structTypes.find(t.name);

        if (iterator != m_structTypes.end())
        {
            return {
                .type = iterator->second,
                .isFloat = false,
                .isUnsigned = false,
                .isVoid = false,
                .structTypeName = t.name,
            };
        }

        Note("; TODO: unknown type '" + t.name + "' lowered as ptr\n");

        return {
            .type = m_ptrTy,
            .isFloat = false,
            .isUnsigned = false,
            .isVoid = false,
            .structTypeName = "",
        };
    }

    static LLVMValueRef ZeroOf(const TypeDesc& typeDesc)
    {
        return LLVMConstNull(typeDesc.type);
    }

    Value ZeroInt() const
    {
        TypeDesc typeDesc{
            .type = I32Ty(),
            .isFloat = false,
            .isUnsigned = false,
            .isVoid = false,
            .structTypeName = "",
        };
        return {
            .value = LLVMConstNull(I32Ty()),
            .typeDesc = typeDesc,
        };
    }

    Value Coerce(Value value, const TypeDesc& target)
    {
        if (!value.typeDesc.type || !target.type || value.typeDesc.type == target.type)
        {
            return value;
        }

        if (!value.typeDesc.structTypeName.empty() || !target.structTypeName.empty())
        {
            return value;
        }

        LLVMValueRef r = nullptr;
        if (!value.typeDesc.isFloat && target.isFloat)
        {
            r = value.typeDesc.isUnsigned ? LLVMBuildUIToFP(m_builder, value.value, target.type, "c")
                                          : LLVMBuildSIToFP(m_builder, value.value, target.type, "c");
        }
        else if (value.typeDesc.isFloat && !target.isFloat)
        {
            r = target.isUnsigned ? LLVMBuildFPToUI(m_builder, value.value, target.type, "c")
                                  : LLVMBuildFPToSI(m_builder, value.value, target.type, "c");
        }
        else if (!value.typeDesc.isFloat && !target.isFloat && !target.isVoid)
        {
            r = LLVMBuildIntCast2(m_builder, value.value, target.type, !target.isUnsigned, "c");
        }
        else
        {
            return value;
        }

        return {
            .value = r,
            .typeDesc = target,
        };
    }

    void DeclareFunction(const FunctionDecl& f)
    {
        FuncInfo info;
        info.returnType = Resolve(f.returnType);

        std::vector<LLVMTypeRef> params;

        for (const auto& p : f.params)
        {
            bool structVal = m_registry.IsUserType(p->type.name) && !m_registry.IsOpaque(p->type.name);
            bool byPtr = p->mod != ParamMod::None || structVal;
            info.paramByPtr.push_back(byPtr);
            params.push_back(byPtr ? m_ptrTy : Resolve(p->type).type);
        }

        info.type = LLVMFunctionType(info.returnType.type, params.data(), static_cast<unsigned>(params.size()), 0);

        if (m_jitMode && f.isExtern)
        {
            LLVMValueRef slot = LLVMAddGlobal(m_mod, m_ptrTy, ("__strata_ext_" + f.name).c_str());
            LLVMSetInitializer(slot, LLVMConstNull(m_ptrTy));
            m_externSlots[f.name] = slot;
            info.function = nullptr;
        }
        else
        {
            info.function = LLVMAddFunction(m_mod, f.mangledName.c_str(), info.type);
        }

        m_funcs[f.mangledName] = info;
    }

    void DefineFunction(const FunctionDecl& f)
    {
        if (!f.body)
        {
            return;
        }

        m_symbols.clear();
        m_terminated = false;
        m_curRet = Resolve(f.returnType);
        m_loops.clear();

        m_curFn = m_funcs[f.mangledName].function;

        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(m_ctx, m_curFn, "entry");
        LLVMPositionBuilderAtEnd(m_builder, entry);

        for (unsigned i = 0; i < f.params.size(); ++i)
        {
            ParamMod mod = f.params[i]->mod;
            TypeDesc typeDesc = Resolve(f.params[i]->type);

            bool structVal = m_registry.IsUserType(f.params[i]->type.name) && !m_registry.IsOpaque(f.params[i]->type.name);

            if (mod != ParamMod::None || structVal)
            {
                // By Ref
                m_symbols[f.params[i]->name] = {
                    .value = LLVMGetParam(m_curFn, i),
                    .typeDesc = typeDesc,
                };
            }
            else
            {
                LLVMValueRef slot = LLVMBuildAlloca(m_builder, typeDesc.type, "arg");
                LLVMBuildStore(m_builder, LLVMGetParam(m_curFn, i), slot);

                m_symbols[f.params[i]->name] = {
                    .value = slot,
                    .typeDesc = typeDesc,
                };
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
                Value value = Coerce(EmitExpr(r->value.get()), m_curRet);
                LLVMBuildRet(m_builder, value.value);
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
            auto* varDecl = static_cast<VarDeclStmt*>(n);

            TypeDesc typeDesc = Resolve(varDecl->type);

            LLVMValueRef slot = LLVMBuildAlloca(m_builder, typeDesc.type, "v");

            if (varDecl->init)
            {
                Value value = Coerce(EmitExpr(varDecl->init.get()), typeDesc);
                LLVMBuildStore(m_builder, value.value, slot);
            }
            else
            {
                LLVMBuildStore(m_builder, ZeroOf(typeDesc), slot);
            }

            m_symbols[varDecl->name] = {
                .value = slot,
                .typeDesc = typeDesc,
            };

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

            m_loops.push_back({
                .cont = condBB,
                .end = endBB,
            });

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
                EmitStmt(fs->init.get());
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

            m_loops.push_back({
                .cont = updBB,
                .end = endBB,
            });

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

    LLVMValueRef ToI1(const Value& v)
    {
        if (v.typeDesc.type == I1Ty())
        {
            return v.value;
        }

        if (v.typeDesc.isFloat)
        {
            return LLVMBuildFCmp(m_builder, LLVMRealONE, v.value, LLVMConstNull(v.typeDesc.type), "tobool");
        }

        return LLVMBuildICmp(m_builder, v.typeDesc.isUnsigned ? LLVMIntNE : LLVMIntNE, v.value,
                             LLVMConstNull(v.typeDesc.type), "tobool");
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

            TypeDesc typeDesc{
                .type = I32Ty(),
                .isFloat = false,
                .isUnsigned = l->isUnsigned,
                .isVoid = false,
                .structTypeName = "",
            };

            return {
                .value = LLVMConstInt(I32Ty(), l->value, 1),
                .typeDesc = typeDesc,
            };
        }

        case NodeKind::FloatLiteral:
        {
            auto* l = static_cast<FloatLiteral*>(n);
            TypeDesc typeDesc{
                .type = LLVMFloatTypeInContext(m_ctx),
                .isFloat = true,
                .isUnsigned = false,
                .isVoid = false,
                .structTypeName = "",
            };

            return {
                .value = LLVMConstReal(LLVMFloatTypeInContext(m_ctx), l->value),
                .typeDesc = typeDesc,
            };
        }

        case NodeKind::BoolLiteral:
            return {
                .value = LLVMConstInt(I1Ty(), static_cast<unsigned long long>(static_cast<BoolLiteral*>(n)->value), 0),
                .typeDesc =
                    {
                        .type = I1Ty(),
                        .isFloat = false,
                        .isUnsigned = false,
                        .isVoid = false,
                        .structTypeName = "",
                    },
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
        case NodeKind::StructInit:
            return EmitStructInit(static_cast<StructInitExpr*>(n));
        default:
            Note("; TODO: LLVM back-end does not lower this expression yet\n");
            return ZeroInt();
        }
    }

    Value EmitIdent(IdentExpr* n)
    {
        auto iterator = m_symbols.find(n->name);

        if (iterator == m_symbols.end())
        {
            Note("; TODO: unknown identifier '" + n->name + "'\n");

            return ZeroInt();
        }

        LLVMValueRef v = LLVMBuildLoad2(m_builder, iterator->second.typeDesc.type, iterator->second.value, "id");

        return {
            .value = v,
            .typeDesc = iterator->second.typeDesc,
        };
    }

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

            auto iterator = m_symbols.find(id->name);

            if (iterator == m_symbols.end())
            {
                return none;
            }

            return {
                .valid = true,
                .ptr = iterator->second.value,
                .typeDesc = iterator->second.typeDesc,
            };
        }

        if (n->kind == NodeKind::Member)
        {
            auto* m = static_cast<MemberExpr*>(n);
            LValue base = EmitLValue(m->base.get());
            if (!base.valid || base.typeDesc.structTypeName.empty())
            {
                return none;
            }

            int idx = m_registry.FieldIndex(base.typeDesc.structTypeName, m->member);
            if (idx < 0)
            {
                return none;
            }

            const auto* st = m_registry.Find(base.typeDesc.structTypeName);

            TypeDesc fieldTypeDesc = Resolve(st->fields[static_cast<std::size_t>(idx)].type);

            LLVMValueRef idxs[2] = {IdxConst(0), IdxConst(static_cast<unsigned>(idx))};
            LLVMValueRef ptr = LLVMBuildGEP2(m_builder, base.typeDesc.type, base.ptr, idxs, 2, "f");

            return {
                .valid = true,
                .ptr = ptr,
                .typeDesc = fieldTypeDesc,
            };
        }

        return none;
    }

    Value EmitMember(MemberExpr* n)
    {
        LValue lvalue = EmitLValue(n);

        if (lvalue.valid)
        {
            LLVMValueRef v = LLVMBuildLoad2(m_builder, lvalue.typeDesc.type, lvalue.ptr, "m");

            return {
                .value = v,
                .typeDesc = lvalue.typeDesc,
            };
        }

        Value base = EmitExpr(n->base.get());

        if (!base.typeDesc.structTypeName.empty())
        {
            int idx = m_registry.FieldIndex(base.typeDesc.structTypeName, n->member);

            if (idx >= 0)
            {
                const auto* st = m_registry.Find(base.typeDesc.structTypeName);

                TypeDesc fieldTypeDesc = Resolve(st->fields[static_cast<std::size_t>(idx)].type);

                LLVMValueRef v = LLVMBuildExtractValue(m_builder, base.value, static_cast<unsigned>(idx), "m");

                return {
                    .value = v,
                    .typeDesc = fieldTypeDesc,
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
            LLVMValueRef r =
                e.typeDesc.isFloat ? LLVMBuildFNeg(m_builder, e.value, "neg") : LLVMBuildNeg(m_builder, e.value, "neg");

            return {
                .value = r,
                .typeDesc = e.typeDesc,
            };
        }

        case UnaryOp::Not:
        {
            LLVMValueRef r = LLVMBuildXor(m_builder, e.value, LLVMConstInt(I1Ty(), 1, 0), "not");

            return {
                .value = r,
                .typeDesc =
                    {
                        .type = I1Ty(),
                        .isFloat = false,
                        .isUnsigned = false,
                        .isVoid = false,
                        .structTypeName = "",
                    },
            };
        }

        case UnaryOp::BitNot:
        {
            LLVMValueRef r = LLVMBuildNot(m_builder, e.value, "bnot");

            return {
                .value = r,
                .typeDesc = e.typeDesc,
            };
        }
        }

        return e;
    }

    Value EmitBinary(BinaryExpr* n)
    {
        Value l = EmitExpr(n->lhs.get());
        Value r = EmitExpr(n->rhs.get());

        TypeDesc typeDesc = l.typeDesc;

        if (l.typeDesc.isFloat && !r.typeDesc.isFloat)
        {
            r = Coerce(r, l.typeDesc);
        }
        else if (!l.typeDesc.isFloat && r.typeDesc.isFloat)
        {
            l = Coerce(l, r.typeDesc);
            typeDesc = r.typeDesc;
        }

        LLVMValueRef out = nullptr;
        bool flt = typeDesc.isFloat;

        if (n->op == BinaryOp::LogicAnd || n->op == BinaryOp::LogicOr)
        {
            const char* op = (n->op == BinaryOp::LogicAnd) ? "and" : "or";

            out = (n->op == BinaryOp::LogicAnd) ? LLVMBuildAnd(m_builder, l.value, r.value, "and")
                                                : LLVMBuildOr(m_builder, l.value, r.value, "or");
            (void)op;

            return {
                .value = out,
                .typeDesc =
                    {
                        .type = I1Ty(),
                        .isFloat = false,
                        .isUnsigned = false,
                        .isVoid = false,
                        .structTypeName = "",
                    },
            };
        }

        auto icmp = [&](const char* pred)
        {
            out = LLVMBuildICmp(m_builder, PredNameToPredicate(pred, typeDesc.isUnsigned), l.value, r.value, "cmp");
        };

        auto fcmp = [&](const char* pred)
        {
            out = LLVMBuildFCmp(m_builder, RealPredNameToPredicate(pred), l.value, r.value, "cmp");
        };

        TypeDesc boolTypeDesc{
            .type = I1Ty(),
            .isFloat = false,
            .isUnsigned = false,
            .isVoid = false,
            .structTypeName = "",
        };

        switch (n->op)
        {
        case BinaryOp::Add:
            out = flt ? LLVMBuildFAdd(m_builder, l.value, r.value, "add")
                      : LLVMBuildAdd(m_builder, l.value, r.value, "add");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Sub:
            out = flt ? LLVMBuildFSub(m_builder, l.value, r.value, "sub")
                      : LLVMBuildSub(m_builder, l.value, r.value, "sub");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Mul:
            out = flt ? LLVMBuildFMul(m_builder, l.value, r.value, "mul")
                      : LLVMBuildMul(m_builder, l.value, r.value, "mul");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Div:
            out = flt ? LLVMBuildFDiv(m_builder, l.value, r.value, "div")
                      : (typeDesc.isUnsigned ? LLVMBuildUDiv(m_builder, l.value, r.value, "div")
                                             : LLVMBuildSDiv(m_builder, l.value, r.value, "div"));
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Mod:
            out = flt ? LLVMBuildFRem(m_builder, l.value, r.value, "mod")
                      : (typeDesc.isUnsigned ? LLVMBuildURem(m_builder, l.value, r.value, "mod")
                                             : LLVMBuildSRem(m_builder, l.value, r.value, "mod"));
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::BitAnd:
            out = LLVMBuildAnd(m_builder, l.value, r.value, "and");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::BitOr:
            out = LLVMBuildOr(m_builder, l.value, r.value, "or");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::BitXor:
            out = LLVMBuildXor(m_builder, l.value, r.value, "xor");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Shl:
            out = LLVMBuildShl(m_builder, l.value, r.value, "shl");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::Shr:
            out = typeDesc.isUnsigned ? LLVMBuildLShr(m_builder, l.value, r.value, "shr")
                                      : LLVMBuildAShr(m_builder, l.value, r.value, "shr");
            return {.value = out, .typeDesc = typeDesc};
        case BinaryOp::EqEq:
            flt ? fcmp("oeq") : icmp("eq");
            return {.value = out, .typeDesc = boolTypeDesc};
        case BinaryOp::NotEq:
            flt ? fcmp("one") : icmp("ne");
            return {.value = out, .typeDesc = boolTypeDesc};
        case BinaryOp::Lt:
            flt ? fcmp("olt") : icmp("lt");
            return {.value = out, .typeDesc = boolTypeDesc};
        case BinaryOp::LtEq:
            flt ? fcmp("ole") : icmp("le");
            return {.value = out, .typeDesc = boolTypeDesc};
        case BinaryOp::Gt:
            flt ? fcmp("ogt") : icmp("gt");
            return {.value = out, .typeDesc = boolTypeDesc};
        case BinaryOp::GtEq:
            flt ? fcmp("oge") : icmp("ge");
            return {.value = out, .typeDesc = boolTypeDesc};
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

    static LLVMRealPredicate RealPredNameToPredicate(const char* p)
    {
        if (std::strcmp(p, "oeq") == 0) return LLVMRealOEQ;
        if (std::strcmp(p, "ogt") == 0) return LLVMRealOGT;
        if (std::strcmp(p, "oge") == 0) return LLVMRealOGE;
        if (std::strcmp(p, "olt") == 0) return LLVMRealOLT;
        if (std::strcmp(p, "ole") == 0) return LLVMRealOLE;
        if (std::strcmp(p, "one") == 0) return LLVMRealONE;
        return LLVMRealOEQ;
    }

    Value EmitAssign(AssignExpr* n)
    {
        Value value = EmitExpr(n->value.get());
        if (n->target->kind == NodeKind::Ident || n->target->kind == NodeKind::Member)
        {
            LValue lvalue = EmitLValue(n->target.get());
            if (lvalue.valid)
            {
                Value stored = Coerce(value, lvalue.typeDesc);
                LLVMBuildStore(m_builder, stored.value, lvalue.ptr);
                return stored;
            }
        }

        Note("; TODO: assignment to unsupported lvalue\n");
        return value;
    }

    Value EmitCall(CallExpr* n)
    {
        if (m_structTypes.contains(n->callee))
        {
            TypeDesc typeDesc = Resolve({.name = n->callee});
            const auto* st = m_registry.Find(n->callee);
            LLVMValueRef agg = LLVMConstNull(typeDesc.type);
            for (std::size_t i = 0; i < n->args.size() && st; ++i)
            {
                if (i >= st->fields.size())
                {
                    break;
                }

                Value argValue = Coerce(EmitExpr(n->args[i].get()), Resolve(st->fields[i].type));
                agg = LLVMBuildInsertValue(m_builder, agg, argValue.value, static_cast<unsigned>(i), "ins");
            }

            return {
                .value = agg,
                .typeDesc = typeDesc,
            };
        }

        auto iterator = m_funcs.find(n->callee);
        if (iterator == m_funcs.end())
        {
            Note("; TODO: call to unknown function '" + n->callee + "'\n");
            return ZeroInt();
        }

        const auto& byPtr = iterator->second.paramByPtr;
        std::vector<LLVMValueRef> args;
        for (std::size_t k = 0; k < n->args.size(); ++k)
        {
            bool passAddr = k < byPtr.size() && byPtr[k];
            args.push_back(passAddr ? ArgAddress(n->args[k].get()) : EmitExpr(n->args[k].get()).value);
        }

        LLVMValueRef callee = iterator->second.function;
        if (m_jitMode)
        {
            auto slotIterator = m_externSlots.find(n->callee);
            if (slotIterator != m_externSlots.end())
            {
                LLVMValueRef fnPtr = LLVMBuildLoad2(m_builder, m_ptrTy, slotIterator->second, "extfn");
                callee = fnPtr;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(m_builder, iterator->second.type, callee, args.data(),
                                           static_cast<unsigned>(args.size()), "call");

        return {
            .value = call,
            .typeDesc = iterator->second.returnType,
        };
    }

    Value EmitStructInit(StructInitExpr* n)
    {
        TypeDesc typeDesc = Resolve({.name = n->typeName});
        const auto* st = m_registry.Find(n->typeName);
        LLVMValueRef agg = LLVMConstNull(typeDesc.type);

        std::size_t positionalIndex = 0;
        for (const auto& field : n->fields)
        {
            std::size_t idx = 0;
            if (field.name.empty())
            {
                idx = positionalIndex++;
            }
            else
            {
                int named = m_registry.FieldIndex(n->typeName, field.name);
                if (named < 0)
                {
                    continue;
                }
                idx = static_cast<std::size_t>(named);
            }

            if (!st || idx >= st->fields.size())
            {
                continue;
            }

            Value fieldValue = Coerce(EmitExpr(field.value.get()), Resolve(st->fields[idx].type));
            agg = LLVMBuildInsertValue(m_builder, agg, fieldValue.value, static_cast<unsigned>(idx), "ins");
        }

        return {
            .value = agg,
            .typeDesc = typeDesc,
        };
    }

    LLVMValueRef ArgAddress(Node* arg)
    {
        LValue lvalue = EmitLValue(arg);
        if (lvalue.valid)
        {
            return lvalue.ptr;
        }

        Value value = EmitExpr(arg);
        LLVMValueRef slot = LLVMBuildAlloca(m_builder, value.typeDesc.type, "outarg");
        LLVMBuildStore(m_builder, value.value, slot);
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
