// Public runtime hooks: panic handler, bounds-violation hook, capabilities and LLVM version.
#include "Embed/EmbedInternal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    const char* strataLLVMVersion(void)
    {
#if STRATA_HAS_LLVM
        static char buf[32];
        unsigned maj = 0;
        unsigned min = 0;
        unsigned pat = 0;
        LLVMGetVersion(&maj, &min, &pat);
        snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min, pat);

        return buf;
#else
    return "disabled";
#endif
    }

    unsigned strataCapabilities(void)
    {
        unsigned capabilities = 0;
#if STRATA_HAS_LLVM
        capabilities |= STRATA_CAP_LLVM_IR | STRATA_CAP_LLVM_AOT | STRATA_CAP_LLVM_JIT;
#endif
        return capabilities;
    }

    static StrataPanicHandler panicHandler = NULL;

    void strata_panic(const char* msg)
    {
        if (panicHandler)
        {
            panicHandler(msg);
        }
        else
        {
            fputs("strata panic: ", stderr);
            fputs(msg, stderr);
            fputc('\n', stderr);
            abort();
        }
    }

    void strata_oob(const char* msg)
    {
        // Reports a recoverable out-of-bounds access (LLVM JIT, boundsCheck on): the panic
        // handler (if any) is notified, but execution continues — the caller gets a dummy
        // element / the write is absorbed. A handler that longjmps still stops the program.
        if (panicHandler)
        {
            panicHandler(msg);
        }
        else
        {
            fputs("strata bounds violation: ", stderr);
            fputs(msg, stderr);
            fputc('\n', stderr);
        }
    }

    void strataSetPanicHandler(StrataPanicHandler handler)
    {
        panicHandler = handler;
    }

#ifdef __cplusplus
}
#endif
