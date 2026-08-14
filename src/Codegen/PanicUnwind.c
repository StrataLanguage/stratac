#include "Codegen/PanicUnwind.h"

#include <stdlib.h>
#include <string.h>

extern void strata_panic(const char* msg);

/* Real CRT setjmp/longjmp entry points. Declared here with raw pointer
   signatures rather than including <setjmp.h>: the CRT declares these
   against a jmp_buf that is an array typedef on most platforms (casting a
   pointer to an array type is not valid C), while the actual ABI passes the
   buffer pointer either way. The pairings follow each toolchain's own
   documented setjmp/longjmp combination:
     - mingw-w64 x64:   _setjmp(buf, frame) + longjmp  (msvcrt exports)
     - mingw-w64 arm64: __mingw_setjmp + __mingw_longjmp
     - MSVC/clang-cl:   _setjmp(buf) + longjmp         (ucrt exports; ucrt
                         has no `_longjmp` symbol, unlike POSIX libcs)
     - POSIX:           _setjmp(buf) + _longjmp
   On x64 the longjmp half is a plain register restore + jump — it does not
   walk unwind tables — which is exactly what skipping JIT'd frames needs.
   The mingw 2-arg _setjmp stores its frame argument in the buffer's Frame
   slot; generated IR calls it with one argument, so the slot gets whatever
   garbage is in RDX. Plain longjmp never reads that slot, so the pair stays
   consistent. */
#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
#if defined(__GNUC__)
extern int __mingw_setjmp(void* buf);
extern void __mingw_longjmp(void* buf, int value);
#define STRATA_SETJMP __mingw_setjmp
#define STRATA_LONGJMP __mingw_longjmp
#else
extern int _setjmp(void* buf);
extern void _longjmp(void* buf, int value);
#define STRATA_SETJMP _setjmp
#define STRATA_LONGJMP _longjmp
#endif
#elif defined(_WIN32) && defined(__GNUC__)
extern int _setjmp(void* buf, void* frame);
extern void longjmp(void* buf, int value);
#define STRATA_SETJMP _setjmp
#define STRATA_LONGJMP longjmp
#elif defined(_WIN32)
extern int _setjmp(void* buf);
extern void longjmp(void* buf, int value);
#define STRATA_SETJMP _setjmp
#define STRATA_LONGJMP longjmp
#else
extern int _setjmp(void* buf);
extern void _longjmp(void* buf, int value);
#define STRATA_SETJMP _setjmp
#define STRATA_LONGJMP _longjmp
#endif

#ifdef _MSC_VER
#define STRATA_TLS __declspec(thread)
#else
#define STRATA_TLS _Thread_local
#endif

static STRATA_TLS StrataUnwindFrame* s_top = NULL;
static STRATA_TLS char s_msg[256] = {0};

void __strata_unwind_push(void* frame)
{
    StrataUnwindFrame* f = (StrataUnwindFrame*)frame;
    f->prev = s_top;
    s_top = f;
}

void __strata_unwind_pop_to(void* frame)
{
    s_top = ((StrataUnwindFrame*)frame)->prev;
}

/* Raises a panic from generated code. When a Strata unwind boundary is
   active (an entry wrapper or a landing-pad frame on this thread), control
   transfers to the innermost frame via longjmp and unwinding proceeds pad
   by pad. With no frame installed (panic inside __strata_module_init, a
   host-direct call, AOT/TCC use), falls back to the legacy behavior:
   notify the handler, then abort if it returned. */
void __strata_raise(const char* msg)
{
    if (msg)
    {
        strncpy(s_msg, msg, sizeof(s_msg) - 1);
        s_msg[sizeof(s_msg) - 1] = '\0';
    }

    if (!s_top)
    {
        strata_panic(msg);
        abort();
    }

    STRATA_LONGJMP(s_top->jmpStorage, 1);
}

/* Continues the unwind from a landing pad: the pad has already dropped its
   frame's owning locals and popped itself from the chain, so `s_top` is the
   caller's frame (or the boundary). */
void __strata_rethrow(void)
{
    if (!s_top)
    {
        strata_panic(s_msg);
        abort();
    }

    STRATA_LONGJMP(s_top->jmpStorage, 1);
}

const char* __strata_panic_message(void)
{
    return s_msg;
}

uintptr_t StrataJitSetJmpAddress(void)
{
    return (uintptr_t)(void*)&STRATA_SETJMP;
}
