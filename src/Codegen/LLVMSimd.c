#include "LLVMSimd.h"

#include <AST/AST.h>

#include "LLVMModuleBuilder.h"
#include <assert.h>
#include <string.h>

#define ER_MALFORMED (-1)

/* Builds the component list from a swizzle string; ER_MALFORMED on error. */
static int BuildComponents(LSimdVecC* buffer, const char* memberAccess)
{
    int count = 0;

    for (int i = 0; i < 4; i++)
    {
        buffer[i] = VC_NULL;
    }

    for (int i = 0; i < 4; i++)
    {
        char ch = memberAccess[i];
        if (ch == '\0')
        {
            break;
        }

        if (ch < 'w' || ch > 'z')
        {
            return ER_MALFORMED;
        }

        switch (ch)
        {
        case 'x':
            buffer[i] = VC_X;
            break;
        case 'y':
            buffer[i] = VC_Y;
            break;
        case 'z':
            buffer[i] = VC_Z;
            break;
        case 'w':
            buffer[i] = VC_W;
            break;
        default:;
        }

        ++count;
    }

    return count;
}

LLVMValueRef LSimdVector2Shuffle(struct Builder* b, LLVMValueRef v0, LLVMValueRef v1, LSimdVecC x, LSimdVecC y)
{
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMValueRef maskV[] = {
        LLVMConstInt(intType, (x != VC_NULL) ? x : VC_X, 0),
        LLVMConstInt(intType, (y != VC_NULL) ? y : VC_Y, 0),
    };

    LLVMValueRef mask = LLVMConstVector(maskV, 2);

    return LLVMBuildShuffleVector(b->m_builder, v0, v1, mask, "shuf");
}

LLVMValueRef LSimdVector4Shuffle(struct Builder* b, LLVMValueRef v0, LLVMValueRef v1, LSimdVecC x, LSimdVecC y,
                                 LSimdVecC z, LSimdVecC w)
{
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMValueRef maskV[] = {
        LLVMConstInt(intType, (x != VC_NULL) ? x : VC_X, 0),
        LLVMConstInt(intType, (y != VC_NULL) ? y : VC_Y, 0),
        LLVMConstInt(intType, (z != VC_NULL) ? z : VC_Z, 0),
        LLVMConstInt(intType, (w != VC_NULL) ? w : VC_W, 0),
    };

    LLVMValueRef mask = LLVMConstVector(maskV, 4);

    return LLVMBuildShuffleVector(b->m_builder, v0, v1, mask, "shuf");
}

LLVMValueRef LSimdVector2Broadcast(struct Builder* b, LLVMValueRef scalar)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    LLVMValueRef poisonVec = LLVMGetPoison(vecType);

    /* Insert scalar into first lane */
    LLVMValueRef singleComp
        = LLVMBuildInsertElement(b->m_builder, poisonVec, scalar, LLVMConstInt(intType, 0, 0), "vecinit");

    /* Reorder to XXXX */
    return LSimdVector2Shuffle(b, singleComp, poisonVec, VC_X, VC_X);
}

LLVMValueRef LSimdVector4Broadcast(struct Builder* b, LLVMValueRef scalar)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    LLVMValueRef poisonVec = LLVMGetPoison(vecType);

    /* Insert scalar into first lane */
    LLVMValueRef singleComp
        = LLVMBuildInsertElement(b->m_builder, poisonVec, scalar, LLVMConstInt(intType, 0, 0), "vecinit");

    /* Reorder to XXXX */
    return LSimdVector4Shuffle(b, singleComp, poisonVec, VC_X, VC_X, VC_X, VC_X);
}

LLVMValueRef LSimdVector2Construct(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    /* Splat scalar to all lanes */
    if (n->args.count == 1)
    {
        Value scalarValue = EmitExpr(b, (Node*)n->args.items[0]);
        return LSimdVector2Broadcast(b, scalarValue.value);
    }

    /* Load float2 */
    else if (n->args.count == 2)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        vec = LLVMBuildInsertElement(b->m_builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");

        return vec;
    }
    else
    {
        DiagErrorFmt(b->m_diag, n->base.range, "invalid call to vector2 construct");
    }

    return NULL;
}

LLVMValueRef LSimdVector4Construct(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->m_ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->m_ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    /* Splat scalar to all lanes */
    if (n->args.count == 1)
    {
        Value scalarValue = EmitExpr(b, (Node*)n->args.items[0]);
        return LSimdVector4Broadcast(b, scalarValue.value);
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
        rhsVec = LSimdVector4Broadcast(b, rhs);
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
        /* Bitcast LHS and RHS to int vectors, OR, then bitcast back to floats */
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

LLVMValueRef LSimdVectorDestructure(struct Builder* b, LLVMValueRef vec, const struct MemberExpr* expr)
{
    LSimdVecC c[4];
    int numComponents = BuildComponents(c, expr->member);

    if (numComponents == ER_MALFORMED)
    {
        DiagError(b->m_diag, expr->base.range, "malformed components in vector destructure");
        return NULL;
    }

    /* Single lane extract */
    if (numComponents == 1)
    {
        LSimdVecC index = c[0];

        /* numComponents should be ER_MALFORMED or 0 if there are no valid components */
        assert(index != VC_NULL);

        LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(b->m_ctx), index, 0);
        return LLVMBuildExtractElement(b->m_builder, vec, idx, "vecext");
    }

    if (numComponents == 3)
    {
        return LSimdVector4Shuffle(b, vec, vec, c[0], c[1], c[2], VC_W);
    }

    if (numComponents == 4)
    {
        return LSimdVector4Shuffle(b, vec, vec, c[0], c[1], c[2], c[3]);
    }

    DiagError(b->m_diag, expr->base.range, "malformed components in vector destructure");
    return NULL;
}
