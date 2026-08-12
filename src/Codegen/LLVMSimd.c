#include "LLVMSimd.h"

#include <AST/AST.h>

#include "LLVMModuleBuilder.h"
#include <assert.h>
#include <string.h>

#if 0
typedef enum LSimdIntrinsic
{
    LSID_MAX,
} LSimdIntrinsic;

static unsigned int intrinIdCache[LSID_MAX];

const char* intrinInstrs[] = {};

static unsigned int LSimdGetIntrinsicID(LSimdIntrinsic id)
{
    assert(id < (sizeof(intrinInstrs) / sizeof(intrinInstrs[0])));
    assert(id >= 0);

    /* Check if the value is not zero- LLVM's `not_intrinsic`, and the default initialized value for statics. */
    if (intrinIdCache[id] > 0)
    {
        return intrinIdCache[id];
    }

    const char* name = intrinInstrs[id];

    const unsigned int intrin = LLVMLookupIntrinsicID(name, strlen(name));
    intrinIdCache[id] = intrin;

    return intrin;
}
#endif

LLVMValueRef LSimdVectorBroadcast(struct Builder* b, LLVMValueRef scalar)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    LLVMValueRef poisonVec = LLVMGetPoison(vecType);

    /* Insert scalar into first lane */
    LLVMValueRef singleComp
        = LLVMBuildInsertElement(b->m_builder, poisonVec, scalar, LLVMConstInt(intType, 0, 0), "vecinit");

    /* Build permute mask XXXX */
    LLVMValueRef maskV[] = {
        LLVMConstInt(intType, 0, 0),
        LLVMConstInt(intType, 0, 0),
        LLVMConstInt(intType, 0, 0),
        LLVMConstInt(intType, 0, 0),
    };

    LLVMValueRef mask = LLVMConstVector(maskV, 4);

    LLVMValueRef dup = LLVMBuildShuffleVector(b->m_builder, singleComp, poisonVec, mask, "dup");
    return dup;
}

LLVMValueRef LSimdVectorConstruct(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    /* Splat scalar to all lanes */
    if (n->args.count == 1)
    {
        Value scalarValue = EmitExpr(b, (Node*)n->args.items[0]);
        return LSimdVectorBroadcast(b, scalarValue.value);
    }

    /* Load float3 */
    else if (n->args.count == 3)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);
        Value z = EmitExpr(b, (Node*)n->args.items[2]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        vec = LLVMBuildInsertElement(b->m_builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, z.value, LLVMConstInt(intType, 2, 0), "vecinit");
        /* Zero the final component */
        vec = LLVMBuildInsertElement(b->m_builder, vec, LLVMConstReal(scalarType, 0.0), LLVMConstInt(intType, 3, 0),
                                     "vecinit");

        return vec;
    }

    /* Load float4 */
    else if (n->args.count == 4)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);
        Value z = EmitExpr(b, (Node*)n->args.items[2]);
        Value w = EmitExpr(b, (Node*)n->args.items[3]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        vec = LLVMBuildInsertElement(b->m_builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, z.value, LLVMConstInt(intType, 2, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, w.value, LLVMConstInt(intType, 3, 0), "vecinit");

        return vec;
    }
    else
    {
        DiagErrorFmt(b->m_diag, n->base.range, "invalid call to vector construct");
    }

    return NULL;
}

LLVMValueRef LSimdVectorBinExpr(Builder* b, LLVMValueRef vec, LLVMValueRef rhs, const struct BinaryExpr* binexp)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    /* Broadcast rhs if it's a scalar, not a vector */
    LLVMValueRef rhsVec = rhs;
    if (LLVMGetTypeKind(LLVMTypeOf(rhs)) != LLVMVectorTypeKind)
    {
        rhsVec = LSimdVectorBroadcast(b, rhs);
    }

    switch (binexp->op)
    {
    case BinAdd:
        return LLVMBuildFAdd(b->m_builder, vec, rhsVec, "");

    case BinSub:
        return LLVMBuildFSub(b->m_builder, vec, rhsVec, "");

    case BinMul:
        return LLVMBuildFMul(b->m_builder, vec, rhsVec, "");

    case BinDiv:
        return LLVMBuildFDiv(b->m_builder, vec, rhsVec, "");

    case BinBitOr:
    {
        /* Bitcast LHS and LHS to int vectors, OR, then bitcast back to floats */
        LLVMTypeRef intVecTy = LLVMVectorType(intType, 4);

        LLVMValueRef lhsInt = LLVMBuildBitCast(b->m_builder, vec, intVecTy, "or.lhs.bc");
        LLVMValueRef rhsInt = LLVMBuildBitCast(b->m_builder, rhsVec, intVecTy, "or.rhs.bc");
        LLVMValueRef orInt = LLVMBuildOr(b->m_builder, lhsInt, rhsInt, "or.tmp");
        return LLVMBuildBitCast(b->m_builder, orInt, vecType, "");
    }
    case BinShl:
    case BinShr:
    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
    case BinLogicAnd:
    case BinLogicOr:
    case BinMod:
    default:;
        DiagErrorFmt(b->m_diag, binexp->base.range, "unsupported operation for vector data type");
        break;
    }

    return NULL; /* unreachable if op is valid */
}
