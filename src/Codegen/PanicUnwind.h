#pragma once

#include <stdint.h>

/* The unwind frame installed by JIT'd functions that own heap values (and by
   the entry wrappers at the host boundary). The layout is shared with the
   LLVM IR emitted by LLVMModuleBuilder (an opaque [62 x i64] jmp-buffer area
   at offset 0, then the chain pointer); keep the two in sync. 62 x 8 = 496
   bytes covers every platform's CRT jmp_buf (MSVC x64 = 256, MSVC arm64 =
   288, glibc/musl <= 216) and the trailing pad keeps the total size a
   multiple of 16 so a 16-aligned alloca keeps the jmp_buf aligned too. */
typedef struct StrataUnwindFrame
{
    uint64_t jmpStorage[62]; /* CRT jmp_buf (host _setjmp/_longjmp write here) */
    struct StrataUnwindFrame* prev;
    uint64_t pad;
} StrataUnwindFrame;

/* Runtime backing called from generated IR. LLVMJit.c maps the `__strata_*`
   symbol names to these addresses. */
void __strata_unwind_push(void* frame);
void __strata_unwind_pop_to(void* frame);
void __strata_raise(const char* msg);
void __strata_rethrow(void);
const char* __strata_panic_message(void);

/* Address of the host CRT setjmp (_setjmp), mapped into the JIT so generated
   prologues can install a frame. Kept behind a function so the CRT's macro/
   typedef games (jmp_buf as an array type, setjmp as a macro) never leak
   into other translation units. */
uintptr_t StrataJitSetJmpAddress(void);

/* Flag-and-return panic pattern: strata_panic records the last panic message
   in thread-local storage (whether or not a handler consumed it).
   __strata_set_pending_panic records; StrataConsumePendingPanic returns 1
   exactly once per recorded panic (storing the message pointer, valid until
   the next panic) and clears the flag. Backs the public strataConsumePanic. */
void __strata_set_pending_panic(const char* msg);
int StrataConsumePendingPanic(const char** outMessage);
