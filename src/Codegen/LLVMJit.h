// Strata compiler: JIT execution via LLVM's MCJIT (C API).
//
// Loads a built LLVM module into an in-process execution engine, compiles it to
// native code, and resolves function addresses that the host can cast to C
// function pointers and call. This is the path a game engine uses to run Strata
// scripts at native speed at load time, with no process spawn or linking step.
//
// ORC v2 (LLVMOrc*) is also available in LLVM-C.dll for future hot-reload /
// lazy compilation; MCJIT is used here for its tiny, stable C-API surface.
//
// Internal header (LLVM-enabled sources only).
#pragma once

#include "LLVMModuleBuilder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace strata
{

class LLVMJit final
{
public:
    LLVMJit() = default;
    ~LLVMJit();

    LLVMJit(const LLVMJit&) = delete;
    LLVMJit& operator=(const LLVMJit&) = delete;

    // Takes ownership of the module (transfers it to the execution engine).
    // Returns true on success; `errorMessage` is set on failure.
    bool Load(BuiltModule bm, std::string& errorMessage);

    // Binds a declared extern function name to a host address. Must be called
    // before getAddress() triggers compilation of the referencing code. Returns
    // false if the name is not declared in the module.
    bool AddSymbol(const char* name, void* addr);

    // Names the script declared `extern` (the host is expected to bind these).
    const std::vector<std::string>& ExternSymbols() const noexcept
    {
        return m_externs;
    }

    // Resolves a function to its native address, or 0 if not present. The first
    // lookup triggers compilation of everything reachable from that function.
    std::uintptr_t GetAddress(const char* name) const;

    bool Valid() const noexcept
    {
        return m_ee != nullptr;
    }

private:
    static void EnsureInitialized();

    LLVMExecutionEngineRef m_ee = nullptr; // owns the module once loaded
    LLVMContextRef m_ctx = nullptr;        // kept alive for the engine's lifetime
    LLVMModuleRef m_mod = nullptr;         // non-owning; engine owns, kept for lookups

    std::vector<std::string> m_externs;
};

} // namespace strata
