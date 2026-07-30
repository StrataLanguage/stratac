// Strata compiler: LLVM module construction (shared by printer/AOT/JIT).
//
// Translates a Strata AST into a live LLVM module via the C API. Handles the
// bootstrap subset: scalar/vector built-ins, user-defined structs (value types
// with member access and positional construction), opaque engine handle types
// (`extern struct`), and functions with `extern` host bindings.
#include "strata/Codegen/LLVMCApi.h"
#include "strata/AST/AST.h"
#include "LLVMModuleBuilder.h"
#include "TypeRegistry.h"
#include "TypeUtil.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace strata {

namespace {

using namespace strata::llvm_c;

LLVMTypeRef scalarLLVMType(LLVMContextRef ctx, const detail::MappedType& t) {
    if (t.isVoid) return LLVMVoidTypeInContext(ctx);
    LLVMTypeRef elem = nullptr;
    if (t.elemIr == "i1") elem = LLVMInt1TypeInContext(ctx);
    else if (t.elemIr == "i32") elem = LLVMInt32TypeInContext(ctx);
    else if (t.elemIr == "half") elem = LLVMHalfTypeInContext(ctx);
    else if (t.elemIr == "float") elem = LLVMFloatTypeInContext(ctx);
    else if (t.elemIr == "double") elem = LLVMDoubleTypeInContext(ctx);
    else elem = LLVMInt32TypeInContext(ctx);
    if (t.isVector()) return LLVMVectorType(elem, static_cast<unsigned>(t.vec));
    return elem;
}

struct TypeDesc {
    LLVMTypeRef ty = nullptr;
    bool isFloat = false;
    bool isUnsigned = false;
    bool isVoid = false;
    std::string structName; // non-empty when this is a user struct value
};

struct Value {
    LLVMValueRef v = nullptr;
    TypeDesc td;
};

struct FuncInfo {
    LLVMValueRef fn = nullptr;
    LLVMTypeRef ty = nullptr;
    TypeDesc ret;
    std::vector<bool> paramByPtr; // params passed by address (out/inout, or extern structs)
};

struct LValue {
    bool ok = false;
    LLVMValueRef ptr = nullptr;
    TypeDesc td;
};

class Builder {
public:
    BuiltModule build(const Module& module, std::string& notes, bool jitMode) {
        notes = "; LLVM C API back-end\n";
        jitMode_ = jitMode;
        ctx_ = LLVMContextCreate();
        mod_ = LLVMModuleCreateWithNameInContext(module.name.c_str(), ctx_);
        builder_ = LLVMCreateBuilderInContext(ctx_);
        ptrTy_ = LLVMPointerTypeInContext(ctx_, 0);

        registry_.build(module);

        // Create all struct type handles first (allows mutual references),
        // then fill in bodies.
        for (const auto& st : registry_.types()) {
            if (st.opaque) {
                structTypes_[st.name] = ptrTy_; // opaque handles are pointer-sized
            } else {
                structTypes_[st.name] =
                    LLVMStructCreateNamed(ctx_, ("struct." + st.name).c_str());
            }
        }
        for (const auto& st : registry_.types()) {
            if (st.opaque) continue;
            std::vector<LLVMTypeRef> fields;
            for (const auto& f : st.fields) fields.push_back(resolve(f.type).ty);
            LLVMStructSetBody(structTypes_[st.name], fields.data(),
                              static_cast<unsigned>(fields.size()), 0);
        }

        for (const auto& f : module.functions) declareFunction(*f);
        std::ostringstream bodyNotes;
        notesSink_ = &bodyNotes;
        for (const auto& f : module.functions) defineFunction(*f);
        notesSink_ = nullptr;

        for (const auto& f : module.functions) {
            if (f->isExtern) externNames_.push_back(f->name);
        }

        notes += bodyNotes.str();

        if (builder_) { LLVMDisposeBuilder(builder_); builder_ = nullptr; }
        BuiltModule out(ctx_, mod_);
        out.externSymbols = std::move(externNames_);
        ctx_ = nullptr;
        mod_ = nullptr;
        return out;
    }

private:
    LLVMContextRef ctx_ = nullptr;
    LLVMModuleRef mod_ = nullptr;
    LLVMBuilderRef builder_ = nullptr;
    LLVMTypeRef ptrTy_ = nullptr;
    TypeRegistry registry_;
    std::map<std::string, LLVMTypeRef> structTypes_;
    std::map<std::string, FuncInfo> funcs_;
    std::map<std::string, Value> symbols_; // v = alloca slot
    std::map<std::string, LLVMValueRef> externSlots_;
    std::vector<std::string> externNames_;
    TypeDesc curRet_;
    bool terminated_ = false;
    bool jitMode_ = false;
    std::ostringstream* notesSink_ = nullptr;
    LLVMValueRef curFn_ = nullptr;
    struct Loop { LLVMBasicBlockRef cont; LLVMBasicBlockRef end; };
    std::vector<Loop> loops_;

    LLVMTypeRef i32Ty() const { return LLVMInt32TypeInContext(ctx_); }
    LLVMTypeRef i1Ty() const { return LLVMInt1TypeInContext(ctx_); }

    LLVMBasicBlockRef newBB(const char* name) {
        return LLVMAppendBasicBlockInContext(ctx_, curFn_, name);
    }
    void positionAtEnd(LLVMBasicBlockRef bb) {
        LLVMPositionBuilderAtEnd(builder_, bb);
        terminated_ = false;
    }
    void br(LLVMBasicBlockRef dest) {
        if (!terminated_) { LLVMBuildBr(builder_, dest); terminated_ = true; }
    }
    LLVMValueRef idxConst(unsigned i) const { return LLVMConstInt(i32Ty(), i, 1); }

    void note(const std::string& s) { if (notesSink_) *notesSink_ << s; }

    TypeDesc resolve(const TypeName& t) {
        auto m = detail::mapType(t);
        if (m.valid) return {scalarLLVMType(ctx_, m), m.isFloat, m.isUnsigned, m.isVoid, ""};
        auto it = structTypes_.find(t.name);
        if (it != structTypes_.end()) return {it->second, false, false, false, t.name};
        note("; TODO: unknown type '" + t.name + "' lowered as ptr\n");
        return {ptrTy_, false, false, false, ""};
    }

    LLVMValueRef zeroOf(TypeDesc td) const { return LLVMConstNull(td.ty); }

    Value zeroInt() const {
        TypeDesc td{i32Ty(), false, false, false, ""};
        return {LLVMConstNull(i32Ty()), td};
    }

    // int<->float coercion for scalars.
    Value coerce(Value v, TypeDesc target) {
        if (!v.td.ty || !target.ty || v.td.ty == target.ty) return v;
        if (!v.td.structName.empty() || !target.structName.empty()) return v;
        LLVMValueRef r = nullptr;
        if (!v.td.isFloat && target.isFloat) {
            r = v.td.isUnsigned ? LLVMBuildUIToFP(builder_, v.v, target.ty, "c")
                                : LLVMBuildSIToFP(builder_, v.v, target.ty, "c");
        } else if (v.td.isFloat && !target.isFloat) {
            r = target.isUnsigned ? LLVMBuildFPToUI(builder_, v.v, target.ty, "c")
                                  : LLVMBuildFPToSI(builder_, v.v, target.ty, "c");
        } else if (!v.td.isFloat && !target.isFloat && !target.isVoid) {
            r = LLVMBuildIntCast2(builder_, v.v, target.ty, !target.isUnsigned, "c");
        } else {
            return v;
        }
        return {r, target};
    }

    void declareFunction(const FunctionDecl& f) {
        FuncInfo info;
        info.ret = resolve(f.returnType);
        std::vector<LLVMTypeRef> params;
        for (const auto& p : f.params) {
            // By pointer when writing back (out/inout), and whenever a struct
            // crosses the host boundary (extern) -- by-value struct passing is
            // ABI-fragile, so externs always take structs by pointer.
            bool structVal = registry_.isUserType(p->type.name) && !registry_.isOpaque(p->type.name);
            bool byPtr = (p->mod == ParamMod::Out || p->mod == ParamMod::InOut) ||
                         (f.isExtern && structVal);
            info.paramByPtr.push_back(byPtr);
            params.push_back(byPtr ? ptrTy_ : resolve(p->type).ty);
        }
        info.ty = LLVMFunctionType(info.ret.ty, params.data(),
                                   static_cast<unsigned>(params.size()), 0);
        if (jitMode_ && f.isExtern) {
            // Externs are not overloadable, so mangledName == name; the host
            // binds the slot by source name.
            LLVMValueRef slot = LLVMAddGlobal(mod_, ptrTy_, ("__strata_ext_" + f.name).c_str());
            LLVMSetInitializer(slot, LLVMConstNull(ptrTy_));
            externSlots_[f.name] = slot;
            info.fn = nullptr;
        } else {
            info.fn = LLVMAddFunction(mod_, f.mangledName.c_str(), info.ty);
        }
        funcs_[f.mangledName] = info;
    }

    void defineFunction(const FunctionDecl& f) {
        if (!f.body) return; // declaration: extern or forward decl
        symbols_.clear();
        terminated_ = false;
        curRet_ = resolve(f.returnType);
        loops_.clear();

        curFn_ = funcs_[f.mangledName].fn;
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx_, curFn_, "entry");
        LLVMPositionBuilderAtEnd(builder_, entry);

        for (unsigned i = 0; i < f.params.size(); ++i) {
            ParamMod mod = f.params[i]->mod;
            TypeDesc td = resolve(f.params[i]->type);
            if (mod == ParamMod::Out || mod == ParamMod::InOut) {
                // The incoming argument is already a pointer to the caller's
                // storage; reads/writes go directly through it.
                symbols_[f.params[i]->name] = {LLVMGetParam(curFn_, i), td};
            } else {
                LLVMValueRef slot = LLVMBuildAlloca(builder_, td.ty, "arg");
                LLVMBuildStore(builder_, LLVMGetParam(curFn_, i), slot);
                symbols_[f.params[i]->name] = {slot, td};
            }
        }

        for (const auto& s : static_cast<Block*>(f.body.get())->statements) {
            emitStmt(s.get());
            if (terminated_) break;
        }
        if (!terminated_) {
            if (curRet_.isVoid) LLVMBuildRetVoid(builder_);
            else LLVMBuildRet(builder_, zeroOf(curRet_));
        }
    }

    void emitStmt(Node* n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Return: {
                auto r = static_cast<ReturnStmt*>(n);
                if (r->value) {
                    Value v = coerce(emitExpr(r->value.get()), curRet_);
                    LLVMBuildRet(builder_, v.v);
                } else {
                    LLVMBuildRetVoid(builder_);
                }
                terminated_ = true;
                return;
            }
            case NodeKind::ExprStmt:
                if (auto e = static_cast<ExprStmt*>(n)->expr.get()) (void)emitExpr(e);
                return;
            case NodeKind::VarDecl: {
                auto vd = static_cast<VarDeclStmt*>(n);
                TypeDesc td = resolve(vd->type);
                LLVMValueRef slot = LLVMBuildAlloca(builder_, td.ty, "v");
                if (vd->init) {
                    Value v = coerce(emitExpr(vd->init.get()), td);
                    LLVMBuildStore(builder_, v.v, slot);
                } else {
                    LLVMBuildStore(builder_, zeroOf(td), slot);
                }
                symbols_[vd->name] = {slot, td};
                return;
            }
            case NodeKind::Block:
                for (auto& s : static_cast<Block*>(n)->statements) emitStmt(s.get());
                return;
            case NodeKind::If: {
                auto i = static_cast<IfStmt*>(n);
                LLVMValueRef cond = toI1(emitExpr(i->condition.get()));
                LLVMBasicBlockRef thenBB = newBB("if.then");
                LLVMBasicBlockRef endBB = newBB("if.end");
                LLVMBasicBlockRef elseBB = i->elseBranch ? newBB("if.else") : endBB;
                LLVMBuildCondBr(builder_, cond, thenBB, elseBB);
                terminated_ = true;
                positionAtEnd(thenBB);
                emitStmt(i->thenBranch.get());
                br(endBB);
                if (i->elseBranch) {
                    positionAtEnd(elseBB);
                    emitStmt(i->elseBranch.get());
                    br(endBB);
                }
                positionAtEnd(endBB);
                return;
            }
            case NodeKind::While: {
                auto w = static_cast<WhileStmt*>(n);
                LLVMBasicBlockRef condBB = newBB("while.cond");
                LLVMBasicBlockRef bodyBB = newBB("while.body");
                LLVMBasicBlockRef endBB = newBB("while.end");
                LLVMBuildBr(builder_, condBB);
                terminated_ = true;
                positionAtEnd(condBB);
                LLVMValueRef cond = toI1(emitExpr(w->condition.get()));
                LLVMBuildCondBr(builder_, cond, bodyBB, endBB);
                terminated_ = true;
                positionAtEnd(bodyBB);
                loops_.push_back({condBB, endBB});
                emitStmt(w->body.get());
                loops_.pop_back();
                br(condBB);
                positionAtEnd(endBB);
                return;
            }
            case NodeKind::For: {
                auto fs = static_cast<ForStmt*>(n);
                if (fs->init) emitStmt(fs->init.get()); // var decl or expression
                LLVMBasicBlockRef condBB = newBB("for.cond");
                LLVMBasicBlockRef bodyBB = newBB("for.body");
                LLVMBasicBlockRef updBB = newBB("for.update");
                LLVMBasicBlockRef endBB = newBB("for.end");
                br(condBB);
                positionAtEnd(condBB);
                if (fs->condition) {
                    LLVMBuildCondBr(builder_, toI1(emitExpr(fs->condition.get())), bodyBB, endBB);
                } else {
                    LLVMBuildBr(builder_, bodyBB);
                }
                terminated_ = true;
                positionAtEnd(bodyBB);
                loops_.push_back({updBB, endBB}); // continue runs the update
                emitStmt(fs->body.get());
                loops_.pop_back();
                br(updBB);
                positionAtEnd(updBB);
                if (fs->update) (void)emitExpr(fs->update.get());
                br(condBB);
                positionAtEnd(endBB);
                return;
            }
            case NodeKind::Break:
                if (!loops_.empty()) br(loops_.back().end);
                return;
            case NodeKind::Continue:
                if (!loops_.empty()) br(loops_.back().cont);
                return;
            default:
                (void)emitExpr(n); // expression-shaped statement
                return;
        }
    }

    // Coerce a value to i1 for use as a branch condition.
    LLVMValueRef toI1(Value v) {
        if (v.td.ty == i1Ty()) return v.v;
        if (v.td.isFloat)
            return LLVMBuildFCmp(builder_, "one", v.v, LLVMConstNull(v.td.ty), "tobool");
        return LLVMBuildICmp(builder_, v.td.isUnsigned ? LLVMIntNE : LLVMIntNE, v.v,
                             LLVMConstNull(v.td.ty), "tobool");
    }

    Value emitExpr(Node* n) {
        if (!n) return zeroInt();
        switch (n->kind) {
            case NodeKind::IntLiteral: {
                auto l = static_cast<IntLiteral*>(n);
                TypeDesc td{i32Ty(), false, l->isUnsigned, false, ""};
                return {LLVMConstInt(i32Ty(), l->value, 1), td};
            }
            case NodeKind::FloatLiteral: {
                auto l = static_cast<FloatLiteral*>(n);
                TypeDesc td{LLVMFloatTypeInContext(ctx_), true, false, false, ""};
                return {LLVMConstReal(LLVMFloatTypeInContext(ctx_), l->value), td};
            }
            case NodeKind::BoolLiteral:
                return {LLVMConstInt(i1Ty(),
                                     static_cast<unsigned long long>(static_cast<BoolLiteral*>(n)->value), 0),
                        {i1Ty(), false, false, false, ""}};
            case NodeKind::Ident:
                return emitIdent(static_cast<IdentExpr*>(n));
            case NodeKind::Unary:
                return emitUnary(static_cast<UnaryExpr*>(n));
            case NodeKind::Binary:
                return emitBinary(static_cast<BinaryExpr*>(n));
            case NodeKind::Assign:
                return emitAssign(static_cast<AssignExpr*>(n));
            case NodeKind::Call:
                return emitCall(static_cast<CallExpr*>(n));
            case NodeKind::Member:
                return emitMember(static_cast<MemberExpr*>(n));
            default:
                note("; TODO: LLVM back-end does not lower this expression yet\n");
                return zeroInt();
        }
    }

    Value emitIdent(IdentExpr* n) {
        auto it = symbols_.find(n->name);
        if (it == symbols_.end()) {
            note("; TODO: unknown identifier '" + n->name + "'\n");
            return zeroInt();
        }
        LLVMValueRef v = LLVMBuildLoad2(builder_, it->second.td.ty, it->second.v, "id");
        return {v, it->second.td};
    }

    // Resolves an lvalue (a variable or a member-access chain rooted at one) to
    // its storage pointer and element type.
    LValue emitLValue(Node* n) {
        LValue none;
        if (!n) return none;
        if (n->kind == NodeKind::Ident) {
            auto id = static_cast<IdentExpr*>(n);
            auto it = symbols_.find(id->name);
            if (it == symbols_.end()) return none;
            return {true, it->second.v, it->second.td};
        }
        if (n->kind == NodeKind::Member) {
            auto m = static_cast<MemberExpr*>(n);
            LValue base = emitLValue(m->base.get());
            if (!base.ok || base.td.structName.empty()) return none;
            int idx = registry_.fieldIndex(base.td.structName, m->member);
            if (idx < 0) return none;
            const auto* st = registry_.find(base.td.structName);
            TypeDesc fieldTd = resolve(st->fields[static_cast<std::size_t>(idx)].type);
            LLVMValueRef idxs[2] = {idxConst(0), idxConst(static_cast<unsigned>(idx))};
            LLVMValueRef ptr = LLVMBuildGEP2(builder_, base.td.ty, base.ptr, idxs, 2, "f");
            return {true, ptr, fieldTd};
        }
        return none;
    }

    Value emitMember(MemberExpr* n) {
        LValue lv = emitLValue(n);
        if (lv.ok) {
            LLVMValueRef v = LLVMBuildLoad2(builder_, lv.td.ty, lv.ptr, "m");
            return {v, lv.td};
        }
        // Rvalue member access on a struct value (e.g. getVec().x).
        Value base = emitExpr(n->base.get());
        if (!base.td.structName.empty()) {
            int idx = registry_.fieldIndex(base.td.structName, n->member);
            if (idx >= 0) {
                const auto* st = registry_.find(base.td.structName);
                TypeDesc fieldTd = resolve(st->fields[static_cast<std::size_t>(idx)].type);
                LLVMValueRef v = LLVMBuildExtractValue(builder_, base.v, static_cast<unsigned>(idx), "m");
                return {v, fieldTd};
            }
        }
        note("; TODO: cannot access member '" + n->member + "'\n");
        return zeroInt();
    }

    Value emitUnary(UnaryExpr* n) {
        Value e = emitExpr(n->operand.get());
        switch (n->op) {
            case UnaryOp::Pos: return e;
            case UnaryOp::Neg: {
                LLVMValueRef r = e.td.isFloat ? LLVMBuildFNeg(builder_, e.v, "neg")
                                              : LLVMBuildNeg(builder_, e.v, "neg");
                return {r, e.td};
            }
            case UnaryOp::Not: {
                LLVMValueRef r = LLVMBuildXor(builder_, e.v, LLVMConstInt(i1Ty(), 1, 0), "not");
                return {r, {i1Ty(), false, false, false, ""}};
            }
            case UnaryOp::BitNot: {
                LLVMValueRef r = LLVMBuildNot(builder_, e.v, "bnot");
                return {r, e.td};
            }
        }
        return e;
    }

    Value emitBinary(BinaryExpr* n) {
        Value l = emitExpr(n->lhs.get());
        Value r = emitExpr(n->rhs.get());
        // Promote mixed int/float operands to float so arithmetic is well-typed.
        TypeDesc td = l.td;
        if (l.td.isFloat && !r.td.isFloat) {
            r = coerce(r, l.td);
        } else if (!l.td.isFloat && r.td.isFloat) {
            l = coerce(l, r.td);
            td = r.td;
        }
        LLVMValueRef out = nullptr;
        bool flt = td.isFloat;

        if (n->op == BinaryOp::LogicAnd || n->op == BinaryOp::LogicOr) {
            const char* op = (n->op == BinaryOp::LogicAnd) ? "and" : "or";
            // non-short-circuit on i1
            out = (n->op == BinaryOp::LogicAnd) ? LLVMBuildAnd(builder_, l.v, r.v, "and")
                                                : LLVMBuildOr(builder_, l.v, r.v, "or");
            (void)op;
            return {out, {i1Ty(), false, false, false, ""}};
        }

        auto icmp = [&](const char* pred) {
            out = LLVMBuildICmp(builder_, predNameToPredicate(pred, td.isUnsigned), l.v, r.v, "cmp");
        };
        auto fcmp = [&](const char* pred) {
            out = LLVMBuildFCmp(builder_, pred, l.v, r.v, "cmp");
        };

        TypeDesc boolTd{i1Ty(), false, false, false, ""};
        switch (n->op) {
            case BinaryOp::Add: out = flt ? LLVMBuildFAdd(builder_, l.v, r.v, "add") : LLVMBuildAdd(builder_, l.v, r.v, "add"); return {out, td};
            case BinaryOp::Sub: out = flt ? LLVMBuildFSub(builder_, l.v, r.v, "sub") : LLVMBuildSub(builder_, l.v, r.v, "sub"); return {out, td};
            case BinaryOp::Mul: out = flt ? LLVMBuildFMul(builder_, l.v, r.v, "mul") : LLVMBuildMul(builder_, l.v, r.v, "mul"); return {out, td};
            case BinaryOp::Div:
                out = flt ? LLVMBuildFDiv(builder_, l.v, r.v, "div")
                          : (td.isUnsigned ? LLVMBuildUDiv(builder_, l.v, r.v, "div")
                                           : LLVMBuildSDiv(builder_, l.v, r.v, "div"));
                return {out, td};
            case BinaryOp::Mod:
                out = flt ? LLVMBuildFRem(builder_, l.v, r.v, "mod")
                          : (td.isUnsigned ? LLVMBuildURem(builder_, l.v, r.v, "mod")
                                           : LLVMBuildSRem(builder_, l.v, r.v, "mod"));
                return {out, td};
            case BinaryOp::BitAnd: out = LLVMBuildAnd(builder_, l.v, r.v, "and"); return {out, td};
            case BinaryOp::BitOr:  out = LLVMBuildOr(builder_, l.v, r.v, "or");  return {out, td};
            case BinaryOp::BitXor: out = LLVMBuildXor(builder_, l.v, r.v, "xor"); return {out, td};
            case BinaryOp::Shl:    out = LLVMBuildShl(builder_, l.v, r.v, "shl"); return {out, td};
            case BinaryOp::Shr:
                out = td.isUnsigned ? LLVMBuildLShr(builder_, l.v, r.v, "shr")
                                    : LLVMBuildAShr(builder_, l.v, r.v, "shr");
                return {out, td};
            case BinaryOp::EqEq:  flt ? fcmp("oeq") : icmp("eq"); return {out, boolTd};
            case BinaryOp::NotEq: flt ? fcmp("one") : icmp("ne"); return {out, boolTd};
            case BinaryOp::Lt:    flt ? fcmp("olt") : icmp("lt"); return {out, boolTd};
            case BinaryOp::LtEq:  flt ? fcmp("ole") : icmp("le"); return {out, boolTd};
            case BinaryOp::Gt:    flt ? fcmp("ogt") : icmp("gt"); return {out, boolTd};
            case BinaryOp::GtEq:  flt ? fcmp("oge") : icmp("ge"); return {out, boolTd};
            default: return l;
        }
    }

    static LLVMIntPredicate predNameToPredicate(const char* p, bool uns) {
        if (std::strcmp(p, "eq") == 0) return uns ? LLVMIntEQ : LLVMIntEQ;
        if (std::strcmp(p, "ne") == 0) return uns ? LLVMIntNE : LLVMIntNE;
        if (std::strcmp(p, "lt") == 0) return uns ? LLVMIntULT : LLVMIntSLT;
        if (std::strcmp(p, "le") == 0) return uns ? LLVMIntULE : LLVMIntSLE;
        if (std::strcmp(p, "gt") == 0) return uns ? LLVMIntUGT : LLVMIntSGT;
        if (std::strcmp(p, "ge") == 0) return uns ? LLVMIntUGE : LLVMIntSGE;
        return LLVMIntEQ;
    }

    Value emitAssign(AssignExpr* n) {
        Value v = emitExpr(n->value.get());
        if (n->target->kind == NodeKind::Ident || n->target->kind == NodeKind::Member) {
            LValue lv = emitLValue(n->target.get());
            if (lv.ok) {
                Value stored = coerce(v, lv.td);
                LLVMBuildStore(builder_, stored.v, lv.ptr);
                return stored;
            }
        }
        note("; TODO: assignment to unsupported lvalue\n");
        return v;
    }

    Value emitCall(CallExpr* n) {
        // Constructor: callee names a struct type.
        if (structTypes_.count(n->callee)) {
            TypeDesc td = resolve({n->callee});
            const auto* st = registry_.find(n->callee);
            LLVMValueRef agg = LLVMGetUndef(td.ty);
            for (std::size_t i = 0; i < n->args.size() && st; ++i) {
                if (i >= st->fields.size()) break;
                Value av = coerce(emitExpr(n->args[i].get()), resolve(st->fields[i].type));
                agg = LLVMBuildInsertValue(builder_, agg, av.v, static_cast<unsigned>(i), "ins");
            }
            return {agg, td};
        }

        auto it = funcs_.find(n->callee);
        if (it == funcs_.end()) {
            note("; TODO: call to unknown function '" + n->callee + "'\n");
            return zeroInt();
        }
        const auto& byPtr = it->second.paramByPtr;
        std::vector<LLVMValueRef> args;
        for (std::size_t k = 0; k < n->args.size(); ++k) {
            bool passAddr = k < byPtr.size() && byPtr[k];
            args.push_back(passAddr ? argAddress(n->args[k].get())
                                    : emitExpr(n->args[k].get()).v);
        }
        LLVMValueRef callee = it->second.fn;
        if (jitMode_) {
            auto sit = externSlots_.find(n->callee);
            if (sit != externSlots_.end()) {
                LLVMValueRef fnPtr = LLVMBuildLoad2(builder_, ptrTy_, sit->second, "extfn");
                callee = fnPtr;
            }
        }
        LLVMValueRef call = LLVMBuildCall2(builder_, it->second.ty, callee,
                                           args.data(), static_cast<unsigned>(args.size()), "call");
        return {call, it->second.ret};
    }

    // Address to pass for an out/inout argument: the lvalue's storage if there
    // is one, otherwise a temporary (writes discarded).
    LLVMValueRef argAddress(Node* arg) {
        LValue lv = emitLValue(arg);
        if (lv.ok) return lv.ptr;
        Value v = emitExpr(arg);
        LLVMValueRef slot = LLVMBuildAlloca(builder_, v.td.ty, "outarg");
        LLVMBuildStore(builder_, v.v, slot);
        return slot;
    }
};

} // namespace

BuiltModule buildLLVMModule(const Module& ast, std::string& notes, bool jitMode) {
    Builder b;
    return b.build(ast, notes, jitMode);
}

} // namespace strata
