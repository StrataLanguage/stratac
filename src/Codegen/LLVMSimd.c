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

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 2);

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

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 2);

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

#define IS_VECTOR(x_) ((x_).typeDesc.isSimdVector == true)

#define IS_SCALAR(x_) ((x_).typeDesc.isSimdVector == false)
#define IS_ALL_SCALAR3(x_, y_, z_) (IS_SCALAR(x_) && IS_SCALAR(y_) && IS_SCALAR(z_))
#define IS_ALL_SCALAR4(x_, y_, z_, w_) (IS_SCALAR(x_) && IS_SCALAR(y_) && IS_SCALAR(z_) && IS_SCALAR(w_))

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

    else if (n->args.count == 2)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        /* float4(float2(1.0, 2.0), float2(3.0, 4.0)) */
        /* float3(float2(1.0, 2.0), 3.0) */

        vec = LLVMBuildInsertElement(b->m_builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
    }

    else if (n->args.count == 3)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);
        Value z = EmitExpr(b, (Node*)n->args.items[2]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        /* float3(1.0, 2.0, 3.0) */

        vec = LLVMBuildInsertElement(b->m_builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->m_builder, vec, z.value, LLVMConstInt(intType, 2, 0), "vecinit");
        /* Zero the final component */
        vec = LLVMBuildInsertElement(b->m_builder, vec, LLVMConstReal(scalarType, 0.0), LLVMConstInt(intType, 3, 0),
                                     "vecinit");

        return vec;
    }

    else if (n->args.count == 4)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);
        Value z = EmitExpr(b, (Node*)n->args.items[2]);
        Value w = EmitExpr(b, (Node*)n->args.items[3]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        /* float4(1.0, 2.0, 3.0, 4.0) */

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

    unsigned laneCount = LLVMGetVectorSize(LLVMTypeOf(vec));
    LLVMTypeRef vecType = LLVMVectorType(scalarType, laneCount);

    /* Broadcast rhs if it's a scalar, not a vector */
    LLVMValueRef rhsVec = rhs;
    if (LLVMGetTypeKind(LLVMTypeOf(rhs)) != LLVMVectorTypeKind)
    {
        LLVMValueRef single
            = LLVMBuildInsertElement(b->m_builder, LLVMGetPoison(vecType), rhs, LLVMConstInt(intType, 0, 0), "splat");

        LLVMValueRef maskV[4];
        for (unsigned i = 0; i < laneCount; i++)
        {
            maskV[i] = LLVMConstInt(intType, i, 0);
        }
        LLVMValueRef mask = LLVMConstVector(maskV, laneCount);
        rhsVec = LLVMBuildShuffleVector(b->m_builder, single, single, mask, "shuf");
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
        LLVMTypeRef intVecTy = LLVMVectorType(intType, laneCount);

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

    if (numComponents == 2)
    {
        return LSimdVector2Shuffle(b, vec, vec, c[0], c[1]);
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

/* Extracts lane `i` of `v` as a scalar. */
static LLVMValueRef SimdExtract(struct Builder* b, LLVMValueRef v, unsigned i)
{
    return LLVMBuildExtractElement(b->m_builder, v, LLVMConstInt(LLVMInt32TypeInContext(b->m_ctx), i, 0), "vecext");
}

/* Builds an `N`-lane vector from `count` scalar components. */
static LLVMValueRef SimdBuildVector(struct Builder* b, LLVMValueRef* comps, unsigned count)
{
    LLVMTypeRef vecType = LLVMVectorType(LLVMFloatTypeInContext(b->m_ctx), count);
    LLVMValueRef result = LLVMGetPoison(vecType);

    for (unsigned i = 0; i < count; i++)
    {
        result = LLVMBuildInsertElement(b->m_builder, result, comps[i], LLVMConstInt(LLVMInt32TypeInContext(b->m_ctx), i, 0),
                                        "vecins");
    }

    return result;
}

LLVMValueRef LSimdVectorDot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB)
{
    LLVMTypeRef floatType = LLVMFloatTypeInContext(b->m_ctx);
    unsigned lanes = LLVMGetVectorSize(LLVMTypeOf(vecA));

    /* Element-wise product, then horizontal (lane-wise) sum of all lanes. */
    LLVMValueRef vecMul = LLVMBuildFMul(b->m_builder, vecA, vecB, "dot_mul");

    LLVMValueRef acc = LLVMConstReal(floatType, 0.0);

    for (unsigned i = 0; i < lanes; i++)
    {
        acc = LLVMBuildFAdd(b->m_builder, acc, SimdExtract(b, vecMul, i), "dot_acc");
    }

    return acc;
}

LLVMValueRef LSimdVectorCross(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB)
{
    unsigned lanes = LLVMGetVectorSize(LLVMTypeOf(vecA));

    LLVMValueRef ax = SimdExtract(b, vecA, 0);
    LLVMValueRef ay = SimdExtract(b, vecA, 1);
    LLVMValueRef bx = SimdExtract(b, vecB, 0);
    LLVMValueRef by = SimdExtract(b, vecB, 1);

    /* float2 cross is the scalar z-component: ax*by - ay*bx. */
    if (lanes == 2)
    {
        return LLVMBuildFSub(b->m_builder, LLVMBuildFMul(b->m_builder, ax, by, ""),
                             LLVMBuildFMul(b->m_builder, ay, bx, ""), "cross_z");
    }

    LLVMValueRef az = SimdExtract(b, vecA, 2);
    LLVMValueRef bz = SimdExtract(b, vecB, 2);

    LLVMValueRef x = LLVMBuildFSub(b->m_builder, LLVMBuildFMul(b->m_builder, ay, bz, ""),
                                   LLVMBuildFMul(b->m_builder, az, by, ""), "cross_x");
    LLVMValueRef y = LLVMBuildFSub(b->m_builder, LLVMBuildFMul(b->m_builder, az, bx, ""),
                                   LLVMBuildFMul(b->m_builder, ax, bz, ""), "cross_y");
    LLVMValueRef z = LLVMBuildFSub(b->m_builder, LLVMBuildFMul(b->m_builder, ax, by, ""),
                                   LLVMBuildFMul(b->m_builder, ay, bx, ""), "cross_z");

    /* float3 is stored as a 4-lane vector with lane 3 zeroed; float4 w = 0. */
    LLVMValueRef zero = LLVMConstReal(LLVMFloatTypeInContext(b->m_ctx), 0.0);
    LLVMValueRef comps[4] = {x, y, z, zero};

    return SimdBuildVector(b, comps, 4);
}

LLVMValueRef LSimdVector3Dot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB)
{
    return LSimdVectorDot(b, vecA, vecB);
}
