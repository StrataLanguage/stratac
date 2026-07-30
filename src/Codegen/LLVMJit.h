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

namespace strata {

class LLVMJit {
public:
    LLVMJit() = default;
    ~LLVMJit();

    LLVMJit(const LLVMJit&) = delete;
    LLVMJit& operator=(const LLVMJit&) = delete;

    // Takes ownership of the module (transfers it to the execution engine).
    // Returns true on success; `errorMessage` is set on failure.
    bool load(BuiltModule bm, std::string& errorMessage);

    // Resolves a function to its native address, or 0 if not present. The first
    // lookup triggers compilation of everything reachable from that function.
    std::uint64_t getAddress(const char* name) const;

    bool valid() const noexcept { return ee_ != nullptr; }

private:
    static void ensureInitialized();

    LLVMExecutionEngineRef ee_ = nullptr; // owns the module once loaded
    LLVMContextRef ctx_ = nullptr;        // kept alive for the engine's lifetime
};

} // namespace strata
