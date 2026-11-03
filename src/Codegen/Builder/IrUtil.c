/* Builder: IR helpers: integer types, blocks, entry allocas, predicates, return widening. */
#include "Codegen/Builder/BuilderInternal.h"

static LLVMIntPredicate PredNameToPredicate(const char* p, bool uns)
{
    if (strcmp(p, "eq") == 0)
    {
        return LLVMIntEQ;
    }

    if (strcmp(p, "ne") == 0)
    {
        return LLVMIntNE;
    }

    if (strcmp(p, "lt") == 0)
    {
        return uns ? LLVMIntULT : LLVMIntSLT;
    }

    if (strcmp(p, "le") == 0)
    {
        return uns ? LLVMIntULE : LLVMIntSLE;
    }

    if (strcmp(p, "gt") == 0)
    {
        return uns ? LLVMIntUGT : LLVMIntSGT;
    }

    if (strcmp(p, "ge") == 0)
    {
        return uns ? LLVMIntUGE : LLVMIntSGE;
    }

    return LLVMIntEQ;
}

static LLVMRealPredicate RealPredNameToPredicate(const char* p)
{
    if (strcmp(p, "oeq") == 0)
    {
        return LLVMRealOEQ;
    }
    if (strcmp(p, "ogt") == 0)
    {
        return LLVMRealOGT;
    }
    if (strcmp(p, "oge") == 0)
    {
        return LLVMRealOGE;
    }
    if (strcmp(p, "olt") == 0)
    {
        return LLVMRealOLT;
    }
    if (strcmp(p, "ole") == 0)
    {
        return LLVMRealOLE;
    }
    if (strcmp(p, "one") == 0)
    {
        return LLVMRealONE;
    }

    return LLVMRealOEQ;
}

LLVMValueRef IcmpByName(LLVMBuilderRef builder, const char* pred, bool isUnsigned, LLVMValueRef l, LLVMValueRef r)
{
    return LLVMBuildICmp(builder, PredNameToPredicate(pred, isUnsigned), l, r, "cmp");
}

LLVMValueRef FcmpByName(LLVMBuilderRef builder, const char* pred, LLVMValueRef l, LLVMValueRef r)
{
    return LLVMBuildFCmp(builder, RealPredNameToPredicate(pred), l, r, "cmp");
}

LLVMTypeRef I32Ty(Builder* b)
{
    return LLVMInt32TypeInContext(b->m_ctx);
}

LLVMTypeRef I64Ty(Builder* b)
{
    return LLVMInt64TypeInContext(b->m_ctx);
}

LLVMTypeRef I1Ty(Builder* b)
{
    return LLVMInt1TypeInContext(b->m_ctx);
}

/*
   Return types smaller than 32-bit integers are returned sign/zero-extended
   in a 32-bit register (the callee is responsible for the extension). The ARM64
   ORC/JIT backend leaves the upper bits in the old wider value, so a C caller
   reading the full register at high optimization levels get garbage.

   Widen the LLVM return type to 32-bits so the register always holds a properly extended value.
*/
LLVMTypeRef WidenRetType(Builder* b, LLVMTypeRef t)
{
    if (t && LLVMGetTypeKind(t) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t) < 32)
    {
        return I32Ty(b);
    }

    return t;
}

/* Extend a value of the semantic return type into the (widened) ABI return
   register: zero-extend unsigned / bool, sign-extend signed variants. */
LLVMValueRef ExtendToRetWidth(Builder* b, LLVMValueRef value, TypeDesc semantic, LLVMTypeRef retLlvm)
{
    if (retLlvm == semantic.type)
    {
        return value;
    }

    if (TypeDescIsUnsigned(&semantic) || semantic.type == I1Ty(b))
    {
        return LLVMBuildZExt(b->m_builder, value, retLlvm, "ret.zext");
    }

    return LLVMBuildSExt(b->m_builder, value, retLlvm, "ret.sext");
}

LLVMBasicBlockRef NewBb(Builder* b, const char* name)
{
    return LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, name);
}

LLVMValueRef EntryAlloca(Builder* b, LLVMTypeRef type, const char* name)
{
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(b->m_builder);

    if (b->m_entryAllocaPt)
    {
        LLVMPositionBuilderBefore(b->m_builder, b->m_entryAllocaPt);
    }
    else
    {
        /* The entry block may already be terminated (e.g. drop code emitted
           on a return before any alloca was created). Insert before the
           terminator so the alloca stays in the entry prologue. */
        LLVMValueRef term = LLVMGetBasicBlockTerminator(b->m_entryBlock);

        if (term)
        {
            LLVMPositionBuilderBefore(b->m_builder, term);
        }
        else
        {
            LLVMPositionBuilderAtEnd(b->m_builder, b->m_entryBlock);
        }
    }

    LLVMValueRef slot = LLVMBuildAlloca(b->m_builder, type, name);
    b->m_entryAllocaPt = slot;

    LLVMPositionBuilderAtEnd(b->m_builder, cur);
    return slot;
}

void PositionAtEnd(Builder* b, LLVMBasicBlockRef bb)
{
    LLVMPositionBuilderAtEnd(b->m_builder, bb);
    b->m_terminated = false;
}

void Br(Builder* b, LLVMBasicBlockRef dest)
{
    if (!b->m_terminated)
    {
        LLVMBuildBr(b->m_builder, dest);
        b->m_terminated = true;
    }
}

/* Integer constant of `ty` from a 64-bit folded value. Compile-time folds carry negative values sign-extended to
   64 bits; LLVMConstInt(ty, v, 0) on a narrower type then leaves the bits above the width set in the constant's
   internal representation. It prints as the right value but compares unequal to the canonical constant, so the
   optimizer folds `icmp eq` of the two to false. Masking to the type's width keeps it canonical. */
LLVMValueRef ConstIntTrunc(LLVMTypeRef ty, ConstUInt v)
{
    unsigned width = LLVMGetIntTypeWidth(ty);

    if (width < 64)
    {
        v &= (1ULL << width) - 1;
    }

    return LLVMConstInt(ty, v, 0);
}

LLVMValueRef IdxConst(Builder* b, unsigned i)
{
    /* LLVM requires i32 for struct-field GEP indices (LLVMBuildGEP2 does not
       validate this; an i64 field index produces malformed IR that crashes
       InstCombine once the JIT runs an optimization pipeline). Sequential
       (pointer/array) indices accept any width, so i32 works everywhere. */
    return LLVMConstInt(I32Ty(b), i, 0);
}

LLVMValueRef SizeOfConst(Builder* b, LLVMTypeRef ty)
{
    LLVMValueRef idx[1] = {LLVMConstInt(I64Ty(b), 1, 0)};
    LLVMValueRef gep = LLVMConstGEP2(ty, LLVMConstNull(b->m_ptrTy), idx, 1);

    return LLVMConstPtrToInt(gep, I64Ty(b));
}

/* JIT out-of-bounds: with boundsCheck on, bad indexes don't panic - reads
   resolve to a dummy element and writes are absorbed. Non-owning elements
   share a zeroed global; owning elements get a fresh dummy in per-site entry
   scratch (dropping whatever a prior OOB event left there). */
LLVMValueRef EntryAllocaZeroed(Builder* b, LLVMTypeRef type, const char* name)
{
    LLVMValueRef before = b->m_entryAllocaPt;
    LLVMValueRef slot = EntryAlloca(b, type, name);
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(b->m_builder);

    /* EntryAlloca inserted the slot just before `before` (or the entry
       terminator); positioning there puts the zeroing store right after it. */
    if (before)
    {
        LLVMPositionBuilderBefore(b->m_builder, before);
    }
    else
    {
        LLVMValueRef term = LLVMGetBasicBlockTerminator(b->m_entryBlock);

        if (term)
        {
            LLVMPositionBuilderBefore(b->m_builder, term);
        }
        else
        {
            LLVMPositionBuilderAtEnd(b->m_builder, b->m_entryBlock);
        }
    }

    LLVMBuildStore(b->m_builder, LLVMConstNull(type), slot);
    LLVMPositionBuilderAtEnd(b->m_builder, cur);
    return slot;
}
