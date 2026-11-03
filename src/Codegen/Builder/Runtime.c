/* Builder: Runtime helpers: alloc/free/panic/oob, bounds checks, exported symbol names. */
#include "Codegen/Builder/BuilderInternal.h"

static bool HasSymbolPrefix(const Builder* b)
{
    return b->m_symbolPrefix && b->m_symbolPrefix[0];
}

/* The linker-visible name of an exported (non-extern) symbol. */
const char* ExportedSymbolName(Builder* b, const char* name)
{
    if (!HasSymbolPrefix(b))
    {
        return name;
    }

    return arena_format(b->m_arena, "%s%s", b->m_symbolPrefix, name);
}

/* With a symbol prefix, module-emitted runtime helpers become module-local so several objects
   (each with its own copy) can link into one binary. */
void MakeHelperModuleLocal(Builder* b, LLVMValueRef function)
{
    if (HasSymbolPrefix(b))
    {
        LLVMSetLinkage(function, LLVMInternalLinkage);
    }
}

/* A runtime helper (`strata_alloc`, ...): the host's, or the module's own when it defines one as
   an `@export` function (a Strata-written runtime). */
static LLVMValueRef RuntimeHelperFn(Builder* b, const char* name, LLVMTypeRef type)
{
    LLVMValueRef existing = LLVMGetNamedFunction(b->m_mod, name);

    return existing ? existing : LLVMAddFunction(b->m_mod, name, type);
}

LLVMValueRef StrataAllocFn(Builder* b)
{
    if (!b->m_allocFn)
    {
        LLVMTypeRef params[1] = {I64Ty(b)};
        b->m_allocFnType = LLVMFunctionType(b->m_ptrTy, params, 1, 0);
        b->m_allocFn = RuntimeHelperFn(b, "strata_alloc", b->m_allocFnType);
    }

    return b->m_allocFn;
}

/* A heap cell for a box's value of `size` bytes, returning the value's address. */
LLVMValueRef AllocBoxCell(Builder* b, LLVMValueRef size, const char* name)
{
    LLVMValueRef args[1] = {size};
    StrataAllocFn(b);
    return LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, name);
}

LLVMValueRef StrataFreeFn(Builder* b)
{
    if (!b->m_freeFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_freeFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_freeFn = RuntimeHelperFn(b, "strata_free", b->m_freeFnType);
    }

    return b->m_freeFn;
}

LLVMValueRef StrataPanicFn(Builder* b)
{
    if (!b->m_panicFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_panicFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_panicFn = RuntimeHelperFn(b, "strata_panic", b->m_panicFnType);
    }

    return b->m_panicFn;
}

static LLVMValueRef StrataOobFn(Builder* b)
{
    if (!b->m_oobFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_oobFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_oobFn = RuntimeHelperFn(b, "strata_oob", b->m_oobFnType);
    }

    return b->m_oobFn;
}

/* A private constant global holding `message`, decayed to a char* (GEP 0,0). */
LLVMValueRef MsgGlobalPtr(Builder* b, const char* prefix, const char* message)
{
    size_t len = strlen(message);
    LLVMValueRef strConst = LLVMConstStringInContext(b->m_ctx, message, (unsigned)len, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);
    char* gName = arena_format(b->m_arena, "%s.%d", prefix, b->m_strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->m_mod, strType, gName);
    LLVMSetInitializer(g, strConst);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
    LLVMValueRef idx[2] = {zero, zero};
    return LLVMConstGEP2(strType, g, idx, 2);
}

/* Emits a call to strata_panic(message) followed by an unreachable terminator.
   Shared by bounds checks, null-extern-call checks, and any other runtime
   abort the profile emits. The current block must be terminated. */
void EmitPanic(Builder* b, const char* message)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".pmsg", message)};
    StrataPanicFn(b);
    LLVMBuildCall2(b->m_builder, b->m_panicFnType, b->m_panicFn, args, 1, "");
    LLVMBuildUnreachable(b->m_builder);
}

/* Reports a recoverable out-of-bounds access (LLVM JIT): notifies the host's
   panic handler via strata_oob but returns, so the caller continues with a
   dummy element. */
void EmitOobReport(Builder* b)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".omsg", "array index out of bounds")};
    StrataOobFn(b);
    LLVMBuildCall2(b->m_builder, b->m_oobFnType, b->m_oobFn, args, 1, "");
}

/* Emits a bounds check: if index >= length, call strata_panic with the given
   message and halt (unreachable). Does nothing if index and length are
   compile-time constants and index < length. Skipped entirely when the
   StrataProfile disables bounds checking. */
void EmitBoundsCheck(Builder* b, LLVMValueRef index, LLVMValueRef length)
{
    if (!b->m_boundsCheck)
    {
        return;
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, index, length, "oob");
    LLVMBasicBlockRef oobBB = NewBb(b, "oob");
    LLVMBasicBlockRef okBB = NewBb(b, "ok");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);

    PositionAtEnd(b, oobBB);
    {
        EmitPanic(b, "array index out of bounds");
    }
    b->m_terminated = true;
    PositionAtEnd(b, okBB);
}

/* Branches to a panic with `message` when `bad` is true, and carries on in the ok block otherwise. */
void EmitPanicIf(Builder* b, LLVMValueRef bad, const char* message)
{
    LLVMBasicBlockRef panicBB = NewBb(b, "arith.panic");
    LLVMBasicBlockRef okBB = NewBb(b, "arith.ok");
    LLVMBuildCondBr(b->m_builder, bad, panicBB, okBB);

    PositionAtEnd(b, panicBB);
    EmitPanic(b, message);
    b->m_terminated = true;
    PositionAtEnd(b, okBB);
}
