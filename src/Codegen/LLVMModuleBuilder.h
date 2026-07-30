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
#include <vector>

namespace strata
{

class BuiltModule
{
  public:
    LLVMContextRef ctx = nullptr;
    LLVMModuleRef mod = nullptr;
    // Names declared `extern` in the source that the host runtime must provide.
    std::vector<std::string> externSymbols;

    BuiltModule() = default;
    BuiltModule(LLVMContextRef c, LLVMModuleRef m) : ctx(c), mod(m)
    {
    }
    BuiltModule(const BuiltModule&) = delete;
    BuiltModule& operator=(const BuiltModule&) = delete;
    BuiltModule(BuiltModule&& o) noexcept : ctx(o.ctx), mod(o.mod), externSymbols(std::move(o.externSymbols))
    {
        o.ctx = nullptr;
        o.mod = nullptr;
    }
    BuiltModule& operator=(BuiltModule&& o) noexcept
    {
        if (this != &o)
        {
            Dispose();
            ctx = o.ctx;
            mod = o.mod;
            externSymbols = std::move(o.externSymbols);
            o.ctx = nullptr;
            o.mod = nullptr;
        }
        return *this;
    }
    ~BuiltModule()
    {
        Dispose();
    }

    void Dispose() noexcept
    {
        if (mod)
        {
            LLVMDisposeModule(mod);
            mod = nullptr;
        }
        if (ctx)
        {
            LLVMContextDispose(ctx);
            ctx = nullptr;
        }
    }
    // Relinquishes ownership of BOTH context and module (caller must keep the
    // context alive for as long as anything references the module).
    void Release(LLVMContextRef& outCtx, LLVMModuleRef& outMod) noexcept
    {
        outCtx = ctx;
        outMod = mod;
        ctx = nullptr;
        mod = nullptr;
    }
    explicit operator bool() const noexcept
    {
        return mod != nullptr;
    }
};

// Builds a live LLVM module from the AST. `notes` accumulates "; TODO" comments
// for constructs the builder does not fully lower. When `jitMode` is true,
// `extern` calls are lowered as indirect calls through a writable per-extern
// pointer slot (named "__strata_ext_<name>") that the host fills at runtime;
// this avoids relying on MCJIT symbol resolution, which is unavailable through
// this LLVM-C.dll. In AOT/text mode, externs are ordinary declarations the
// downstream linker resolves.
BuiltModule BuildLlvmModule(const Module& ast, std::string& notes, bool jitMode = false);

} // namespace strata
