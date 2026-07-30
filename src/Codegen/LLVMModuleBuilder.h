// Strata compiler: build a *live* LLVM module from a Strata AST.
//
// This factors the IR construction (shared by the printer, the AOT object
// emitter, and the JIT) out of the back-end. The returned BuiltModule owns its
// LLVMContext and LLVMModule and gives them up on release()/move so an
// ExecutionEngine or TargetMachine can take ownership.
//
// Internal header (LLVM-enabled sources only).
#pragma once

#include "strata/AST/AST.h"
#include "strata/Codegen/LLVMCApi.h"

#include <string>

namespace strata {

class BuiltModule {
public:
    LLVMContextRef ctx = nullptr;
    LLVMModuleRef mod = nullptr;

    BuiltModule() = default;
    BuiltModule(LLVMContextRef c, LLVMModuleRef m) : ctx(c), mod(m) {}
    BuiltModule(const BuiltModule&) = delete;
    BuiltModule& operator=(const BuiltModule&) = delete;
    BuiltModule(BuiltModule&& o) noexcept : ctx(o.ctx), mod(o.mod) {
        o.ctx = nullptr; o.mod = nullptr;
    }
    BuiltModule& operator=(BuiltModule&& o) noexcept {
        if (this != &o) {
            dispose();
            ctx = o.ctx; mod = o.mod;
            o.ctx = nullptr; o.mod = nullptr;
        }
        return *this;
    }
    ~BuiltModule() { dispose(); }

    void dispose() noexcept {
        if (mod) { LLVMDisposeModule(mod); mod = nullptr; }
        if (ctx) { LLVMContextDispose(ctx); ctx = nullptr; }
    }
    // Relinquishes ownership of BOTH context and module (caller must keep the
    // context alive for as long as anything references the module).
    void release(LLVMContextRef& outCtx, LLVMModuleRef& outMod) noexcept {
        outCtx = ctx; outMod = mod; ctx = nullptr; mod = nullptr;
    }
    explicit operator bool() const noexcept { return mod != nullptr; }
};

// Builds a live LLVM module from the AST. `notes` accumulates "; TODO" comments
// for constructs the builder does not fully lower. Returns an invalid module on
// parse-level problems (caller should check diag.hasErrors() first).
BuiltModule buildLLVMModule(const Module& ast, std::string& notes);

} // namespace strata
