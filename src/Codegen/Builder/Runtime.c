// Builder: Runtime helpers: alloc/free/panic/oob, bounds checks, exported symbol names.
#include "Codegen/Builder/BuilderInternal.h"

static bool HasSymbolPrefix(const Builder* b)
{
    return b->symbolPrefix && b->symbolPrefix[0];
}

// The linker-visible name of an exported (non-extern) symbol.
const char* ExportedSymbolName(Builder* b, const char* name)
{
    if (!HasSymbolPrefix(b))
    {
        return name;
    }

    return ArenaFormat(b->arena, "%s%s", b->symbolPrefix, name);
}

// With a symbol prefix, module-emitted runtime helpers become module-local so several objects
// (each with its own copy) can link into one binary.
void MakeHelperModuleLocal(Builder* b, LLVMValueRef function)
{
    if (HasSymbolPrefix(b))
    {
        LLVMSetLinkage(function, LLVMInternalLinkage);
    }
}

// A runtime helper (`strata_alloc`, ...): the host's, or the module's own when it defines one as
// an `@export` function (a Strata-written runtime).
static LLVMValueRef RuntimeHelperFn(Builder* b, const char* name, LLVMTypeRef type)
{
    LLVMValueRef existing = LLVMGetNamedFunction(b->mod, name);

    return existing ? existing : LLVMAddFunction(b->mod, name, type);
}

LLVMValueRef StrataAllocFn(Builder* b)
{
    if (!b->allocFn)
    {
        LLVMTypeRef params[1] = {I64Ty(b)};
        b->allocFnType = LLVMFunctionType(b->ptrTy, params, 1, 0);
        b->allocFn = RuntimeHelperFn(b, "strata_alloc", b->allocFnType);
    }

    return b->allocFn;
}

// A heap cell for a box's value of `size` bytes, returning the value's address.
LLVMValueRef AllocBoxCell(Builder* b, LLVMValueRef size, const char* name)
{
    LLVMValueRef args[1] = {size};
    StrataAllocFn(b);
    return LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, args, 1, name);
}

LLVMValueRef StrataFreeFn(Builder* b)
{
    if (!b->freeFn)
    {
        LLVMTypeRef params[1] = {b->ptrTy};
        b->freeFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->ctx), params, 1, 0);
        b->freeFn = RuntimeHelperFn(b, "strata_free", b->freeFnType);
    }

    return b->freeFn;
}

LLVMValueRef StrataPanicFn(Builder* b)
{
    if (!b->panicFn)
    {
        LLVMTypeRef params[1] = {b->ptrTy};
        b->panicFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->ctx), params, 1, 0);
        b->panicFn = RuntimeHelperFn(b, "strata_panic", b->panicFnType);
    }

    return b->panicFn;
}

static LLVMValueRef StrataOobFn(Builder* b)
{
    if (!b->oobFn)
    {
        LLVMTypeRef params[1] = {b->ptrTy};
        b->oobFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->ctx), params, 1, 0);
        b->oobFn = RuntimeHelperFn(b, "strata_oob", b->oobFnType);
    }

    return b->oobFn;
}

// A private constant global holding `message`, decayed to a char* (GEP 0,0).
LLVMValueRef MsgGlobalPtr(Builder* b, const char* prefix, const char* message)
{
    size_t len = strlen(message);
    LLVMValueRef strConst = LLVMConstStringInContext(b->ctx, message, (unsigned)len, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);
    char* gName = ArenaFormat(b->arena, "%s.%d", prefix, b->strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->mod, strType, gName);
    LLVMSetInitializer(g, strConst);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
    LLVMValueRef idx[2] = {zero, zero};
    return LLVMConstGEP2(strType, g, idx, 2);
}

// Emits a call to strata_panic(message) followed by an unreachable terminator.
// Shared by bounds checks, null-extern-call checks, and any other runtime
// abort the profile emits. The current block must be terminated.
void EmitPanic(Builder* b, const char* message)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".pmsg", message)};
    StrataPanicFn(b);
    LLVMBuildCall2(b->builder, b->panicFnType, b->panicFn, args, 1, "");
    LLVMBuildUnreachable(b->builder);
}

// Reports a recoverable out-of-bounds access (LLVM JIT): notifies the host's
// panic handler via strata_oob but returns, so the caller continues with a
// dummy element.
void EmitOobReport(Builder* b)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".omsg", "array index out of bounds")};
    StrataOobFn(b);
    LLVMBuildCall2(b->builder, b->oobFnType, b->oobFn, args, 1, "");
}

// Emits a bounds check: if index >= length, call strata_panic with the given
// message and halt (unreachable). Does nothing if index and length are
// compile-time constants and index < length. Skipped entirely when the
// StrataProfile disables bounds checking.
void EmitBoundsCheck(Builder* b, LLVMValueRef index, LLVMValueRef length)
{
    if (!b->boundsCheck)
    {
        return;
    }

    LLVMValueRef oob = LLVMBuildICmp(b->builder, LLVMIntUGE, index, length, "oob");
    LLVMBasicBlockRef oobBB = NewBb(b, "oob");
    LLVMBasicBlockRef okBB = NewBb(b, "ok");
    LLVMBuildCondBr(b->builder, oob, oobBB, okBB);

    PositionAtEnd(b, oobBB);
    {
        EmitPanic(b, "array index out of bounds");
    }
    b->terminated = true;
    PositionAtEnd(b, okBB);
}

// Branches to a panic with `message` when `bad` is true, and carries on in the ok block otherwise.
void EmitPanicIf(Builder* b, LLVMValueRef bad, const char* message)
{
    LLVMBasicBlockRef panicBB = NewBb(b, "arith.panic");
    LLVMBasicBlockRef okBB = NewBb(b, "arith.ok");
    LLVMBuildCondBr(b->builder, bad, panicBB, okBB);

    PositionAtEnd(b, panicBB);
    EmitPanic(b, message);
    b->terminated = true;
    PositionAtEnd(b, okBB);
}
