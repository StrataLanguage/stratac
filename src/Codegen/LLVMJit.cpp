// Strata compiler: MCJIT implementation.
#include "LLVMJit.h"
#include "strata/Codegen/LLVMCApi.h"

#include <mutex>

namespace strata {

namespace {
using namespace strata::llvm_c;

void ensureX86Initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        // MCJIT emits to memory via the JIT emitter; the asm printer is not
        // required, but initializing it is harmless and keeps AOT/JIT symmetric.
        LLVMInitializeX86AsmPrinter();
    });
}
} // namespace

void LLVMJit::ensureInitialized() { ensureX86Initialized(); }

LLVMJit::~LLVMJit() {
    // Dispose the engine first (it owns the module), then the context.
    if (ee_) { LLVMDisposeExecutionEngine(ee_); ee_ = nullptr; }
    if (ctx_) { LLVMContextDispose(ctx_); ctx_ = nullptr; }
}

bool LLVMJit::load(BuiltModule bm, std::string& errorMessage) {
    ensureInitialized();
    if (!bm.mod) { errorMessage = "no module to JIT"; return false; }

    LLVMContextRef c = nullptr;
    LLVMModuleRef m = nullptr;
    bm.release(c, m); // steal ownership; bm won't dispose
    ctx_ = c;

    char* err = nullptr;
    if (LLVMCreateExecutionEngineForModule(&ee_, m, &err)) {
        errorMessage = std::string("could not create execution engine: ") +
                       (err ? err : "(no message)");
        if (err) LLVMDisposeMessage(err);
        // On failure the engine did not take ownership; free the module/context.
        LLVMDisposeModule(m);
        ee_ = nullptr;
        LLVMContextDispose(ctx_);
        ctx_ = nullptr;
        return false;
    }
    return true;
}

std::uint64_t LLVMJit::getAddress(const char* name) const {
    if (!ee_ || !name) return 0;
    return LLVMGetFunctionAddress(ee_, name);
}

} // namespace strata
