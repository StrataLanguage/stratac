// Strata compiler: MCJIT implementation.
#include "LLVMJit.h"
#include "strata/Codegen/LLVMCApi.h"

#include <mutex>

namespace strata
{

namespace
{
using namespace strata::llvm_c;

void EnsureX86Initialized()
{
    static std::once_flag flag;
    std::call_once(flag,
                   []
                   {
                       LLVMInitializeX86TargetInfo();
                       LLVMInitializeX86Target();
                       LLVMInitializeX86TargetMC();
                       // MCJIT emits to memory via the JIT emitter; the asm printer is not
                       // required, but initializing it is harmless and keeps AOT/JIT symmetric.
                       LLVMInitializeX86AsmPrinter();
                   });
}
} // namespace

void LLVMJit::EnsureInitialized()
{
    EnsureX86Initialized();
}

LLVMJit::~LLVMJit()
{
    // Dispose the engine first (it owns the module), then the context.
    if (m_ee)
    {
        LLVMDisposeExecutionEngine(m_ee);
        m_ee = nullptr;
    }

    if (m_ctx)
    {
        LLVMContextDispose(m_ctx);
        m_ctx = nullptr;
    }
}

bool LLVMJit::Load(BuiltModule bm, std::string& errorMessage)
{
    EnsureInitialized();
    if (!bm.mod)
    {
        errorMessage = "no module to JIT";
        return false;
    }

    m_externs = bm.externSymbols; // copy before we move bm
    LLVMContextRef c = nullptr;
    LLVMModuleRef m = nullptr;
    bm.Release(c, m); // steal ownership; bm won't dispose
    m_ctx = c;

    char* err = nullptr;
    if (LLVMCreateExecutionEngineForModule(&m_ee, m, &err))
    {
        errorMessage = std::string("could not create execution engine: ") + (err ? err : "(no message)");
        if (err)
        {
            LLVMDisposeMessage(err);
        }

        // On failure the engine did not take ownership; free the module/context.
        LLVMDisposeModule(m);
        m_ee = nullptr;
        LLVMContextDispose(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    m_mod = m; // engine owns it; keep a non-owning ref for symbol lookups
    return true;
}

bool LLVMJit::AddSymbol(const char* name, void* addr)
{
    if (!m_ee || !name || !addr)
    {
        return false;
    }

    // In JIT mode the builder emitted a writable per-extern slot named
    // "__strata_ext_<name>"; resolve its address and store the host pointer.
    std::string slot = std::string("__strata_ext_") + name;
    uint64_t slotAddr = LLVMGetGlobalValueAddress(m_ee, slot.c_str());
    if (slotAddr == 0)
    {
        return false;
    }

    *reinterpret_cast<void**>(slotAddr) = addr;
    return true;
}

std::uint64_t LLVMJit::GetAddress(const char* name) const
{
    if (!m_ee || !name)
    {
        return 0;
    }

    return LLVMGetFunctionAddress(m_ee, name);
}

} // namespace strata
