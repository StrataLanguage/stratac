#include "Codegen/PanicUnwind.h"

#include <stdlib.h>
#include <string.h>

extern void strata_panic(const char* msg);

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

static STRATA_TLS char s_pendingMsg[256] = {0};
static STRATA_TLS int s_pendingSet = 0;

void __strata_set_pending_panic(const char* msg)
{
    if (msg)
    {
        strncpy(s_pendingMsg, msg, sizeof(s_pendingMsg) - 1);
        s_pendingMsg[sizeof(s_pendingMsg) - 1] = '\0';
    }
    else
    {
        s_pendingMsg[0] = '\0';
    }

    s_pendingSet = 1;
}

int StrataConsumePendingPanic(const char** outMessage)
{
    if (!s_pendingSet)
    {
        if (outMessage)
        {
            *outMessage = NULL;
        }

        return 0;
    }

    s_pendingSet = 0;

    if (outMessage)
    {
        *outMessage = s_pendingMsg;
    }

    return 1;
}

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

// MSVC:  error C7552: '_setjmp': purely intrinsic functions have no address
#if defined(_MSC_VER)
static int msvc_setjmp_wrapper(void* buf)
{
    return STRATA_SETJMP(buf);
}

uintptr_t StrataJitSetJmpAddress(void)
{
    return (uintptr_t)(void*)&msvc_setjmp_wrapper;
}
#else
uintptr_t StrataJitSetJmpAddress(void)
{
    return (uintptr_t)(void*)&STRATA_SETJMP;
}
#endif
