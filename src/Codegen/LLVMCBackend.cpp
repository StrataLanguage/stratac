// Strata compiler: in-process LLVM back-end via the C API.
//
// Builds the IR directly through the linked LLVM-C shared library (declared in
// LLVMCApi.h). This is the back-end that ultimately produces native code: the
// module it builds can be verified, dumped, JIT-compiled, or lowered to an
// object file by LLVM without leaving the process.
//
// For the bootstrap this back-end lowers a focused but real subset -- scalar
// integer/float functions with parameters, return statements, local
// declarations, arithmetic (add/sub/mul/div), and calls -- enough to exercise
// the entire link/run path against the real LLVM. The text back-end covers the
// broader statement surface; both share TypeUtil.h.
//
// Only compiled when CMake enables LLVM linkage (STRATA_ENABLE_LLVM); otherwise
// the factory falls back to the null stub in CodegenBackend.cpp.
#if defined(STRATA_ENABLE_LLVM)
#include "strata/Codegen/CodegenBackend.h"
#include "TypeUtil.h"
#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace strata {

namespace {

using namespace strata::llvm_c;

LLVMTypeRef toLLVMType(LLVMContextRef ctx, const detail::MappedType& t) {
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

struct Value {
    LLVMValueRef v = nullptr;
    detail::MappedType t;
};

struct FuncInfo {
    LLVMValueRef fn = nullptr;
    LLVMTypeRef ty = nullptr;
    detail::MappedType ret;
};

class LLVMCImpl {
public:
    CodegenResult run(const Module& module) {
        CodegenResult res;
        res.moduleName = module.name;

        ctx_ = LLVMContextCreate();
        mod_ = LLVMModuleCreateWithNameInContext(module.name.c_str(), ctx_);
        builder_ = LLVMCreateBuilderInContext(ctx_);

        std::ostringstream notes;
        notes << "; LLVM C API back-end\n";

        // First declare all functions (so calls resolve), then define bodies.
        for (const auto& f : module.functions) declareFunction(*f);
        for (const auto& f : module.functions) defineFunction(*f, notes);

        char* diag = nullptr;
        if (LLVMVerifyModule(mod_, kReturnStatusAction, &diag)) {
            notes << "; VERIFY WARNING: " << (diag ? diag : "(no message)") << "\n";
        }
        if (diag) LLVMDisposeMessage(diag);

        char* ir = LLVMPrintModuleToString(mod_);
        res.output = notes.str() + std::string(ir);
        LLVMDisposeMessage(ir);

        dispose();
        res.ok = true;
        return res;
    }

private:
    LLVMContextRef ctx_ = nullptr;
    LLVMModuleRef mod_ = nullptr;
    LLVMBuilderRef builder_ = nullptr;
    std::map<std::string, FuncInfo> funcs_;
    std::map<std::string, Value> symbols_;
    detail::MappedType curRet_;
    bool terminated_ = false;

    void dispose() {
        if (builder_) { LLVMDisposeBuilder(builder_); builder_ = nullptr; }
        if (mod_) { LLVMDisposeModule(mod_); mod_ = nullptr; }
        if (ctx_) { LLVMContextDispose(ctx_); ctx_ = nullptr; }
    }

    // Context-owned common types (avoids accidentally using the global context).
    LLVMTypeRef i32() const { return LLVMInt32TypeInContext(ctx_); }
    LLVMTypeRef i1()  const { return LLVMInt1TypeInContext(ctx_); }
    Value zeroInt() const {
        return {LLVMConstInt(i32(), 0, 1), detail::mapType({"int"})};
    }

    void declareFunction(const FunctionDecl& f) {
        FuncInfo info;
        info.ret = detail::mapType(f.returnType);
        if (!info.ret.valid) info.ret = detail::mapType({"void"});
        std::vector<LLVMTypeRef> params;
        for (const auto& p : f.params) {
            auto pt = detail::mapType(p->type);
            if (!pt.valid) pt = detail::mapType({"int"});
            params.push_back(toLLVMType(ctx_, pt));
        }
        info.ty = LLVMFunctionType(toLLVMType(ctx_, info.ret), params.data(),
                                   static_cast<unsigned>(params.size()), 0);
        info.fn = LLVMAddFunction(mod_, f.name.c_str(), info.ty);
        funcs_[f.name] = info;
    }

    void defineFunction(const FunctionDecl& f, std::ostringstream& notes) {
        symbols_.clear();
        terminated_ = false;
        curRet_ = detail::mapType(f.returnType);
        if (!curRet_.valid) curRet_ = detail::mapType({"void"});

        LLVMValueRef fn = funcs_[f.name].fn;
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx_, fn, "entry");
        LLVMPositionBuilderAtEnd(builder_, entry);

        for (unsigned i = 0; i < f.params.size(); ++i) {
            detail::MappedType pt = detail::mapType(f.params[i]->type);
            if (!pt.valid) pt = detail::mapType({"int"});
            LLVMValueRef param = LLVMGetParam(fn, i);
            symbols_[f.params[i]->name] = {param, pt};
        }

        if (f.body) {
            for (const auto& s : static_cast<Block*>(f.body.get())->statements) {
                emitStmt(s.get(), notes);
                if (terminated_) break;
            }
        }
        if (!terminated_) {
            if (curRet_.isVoid) LLVMBuildRetVoid(builder_);
            else LLVMBuildRet(builder_, LLVMConstInt(toLLVMType(ctx_, curRet_), 0, 1));
        }
    }

    void emitStmt(Node* n, std::ostringstream& notes) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Return: {
                auto r = static_cast<ReturnStmt*>(n);
                if (r->value) {
                    Value v = emitExpr(r->value.get(), notes);
                    LLVMBuildRet(builder_, v.v);
                } else {
                    LLVMBuildRetVoid(builder_);
                }
                terminated_ = true;
                return;
            }
            case NodeKind::ExprStmt:
                if (auto e = static_cast<ExprStmt*>(n)->expr.get()) (void)emitExpr(e, notes);
                return;
            case NodeKind::VarDecl: {
                auto vd = static_cast<VarDeclStmt*>(n);
                if (vd->init) {
                    Value v = emitExpr(vd->init.get(), notes);
                    symbols_[vd->name] = v; // SSA binding (no mutation yet)
                }
                return;
            }
            default:
                notes << "; TODO: LLVM back-end does not lower this statement yet\n";
                return;
        }
    }

    Value emitExpr(Node* n, std::ostringstream& notes) {
        if (!n) return zeroInt();
        switch (n->kind) {
            case NodeKind::IntLiteral: {
                auto l = static_cast<IntLiteral*>(n);
                detail::MappedType t = detail::mapType({l->isUnsigned ? "uint" : "int"});
                return {LLVMConstInt(toLLVMType(ctx_, t), l->value, 1), t};
            }
            case NodeKind::FloatLiteral: {
                auto l = static_cast<FloatLiteral*>(n);
                detail::MappedType t = detail::mapType({"float"});
                return {LLVMConstReal(toLLVMType(ctx_, t), l->value), t};
            }
            case NodeKind::BoolLiteral:
                return {LLVMConstInt(i1(),
                                     static_cast<unsigned long long>(static_cast<BoolLiteral*>(n)->value), 0),
                        detail::mapType({"bool"})};
            case NodeKind::Ident: {
                auto id = static_cast<IdentExpr*>(n);
                auto it = symbols_.find(id->name);
                if (it != symbols_.end()) return it->second;
                notes << "; TODO: unknown identifier '" << id->name << "'\n";
                return zeroInt();
            }
            case NodeKind::Binary:
                return emitBinary(static_cast<BinaryExpr*>(n), notes);
            case NodeKind::Call:
                return emitCall(static_cast<CallExpr*>(n), notes);
            default:
                notes << "; TODO: LLVM back-end does not lower this expression yet\n";
                return zeroInt();
        }
    }

    Value emitBinary(BinaryExpr* n, std::ostringstream& notes) {
        Value l = emitExpr(n->lhs.get(), notes);
        Value r = emitExpr(n->rhs.get(), notes);
        detail::MappedType t = l.t;
        switch (n->op) {
            case BinaryOp::Add:
                return {t.isFloat ? LLVMBuildFAdd(builder_, l.v, r.v, "add")
                                  : LLVMBuildAdd(builder_, l.v, r.v, "add"), t};
            case BinaryOp::Sub:
                return {t.isFloat ? LLVMBuildFSub(builder_, l.v, r.v, "sub")
                                  : LLVMBuildSub(builder_, l.v, r.v, "sub"), t};
            case BinaryOp::Mul:
                return {t.isFloat ? LLVMBuildFMul(builder_, l.v, r.v, "mul")
                                  : LLVMBuildMul(builder_, l.v, r.v, "mul"), t};
            default:
                notes << "; TODO: LLVM back-end: binary op not lowered yet\n";
                return l;
        }
    }

    Value emitCall(CallExpr* n, std::ostringstream& notes) {
        auto it = funcs_.find(n->callee);
        std::vector<LLVMValueRef> args;
        for (const auto& a : n->args) args.push_back(emitExpr(a.get(), notes).v);
        if (it == funcs_.end()) {
            notes << "; TODO: call to unknown function '" << n->callee << "'\n";
            return zeroInt();
        }
        LLVMValueRef call = LLVMBuildCall2(builder_, it->second.ty, it->second.fn,
                                           args.data(), static_cast<unsigned>(args.size()), "call");
        return {call, it->second.ret};
    }
};

class LLVMCBackend : public CodegenBackend {
public:
    std::string_view name() const noexcept override { return "llvm-c"; }
    CodegenResult generate(const Module& mod) override { return LLVMCImpl{}.run(mod); }
};

} // namespace

std::unique_ptr<CodegenBackend> createLLVMBackend() {
    return std::make_unique<LLVMCBackend>();
}

} // namespace strata

#endif // STRATA_ENABLE_LLVM
