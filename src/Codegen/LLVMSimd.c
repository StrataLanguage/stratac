// SIMD vector lowering to LLVM vector IR.
#include "LLVMSimd.h"

#include <AST/AST.h>

#include "LLVMModuleBuilder.h"
#include "TypeDescFlags.h"
#include <assert.h>
#include <string.h>

#define ER_MALFORMED (-1)

// Builds the component list from a swizzle string; ER_MALFORMED on error.
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
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMValueRef maskV[] = {
        LLVMConstInt(intType, (x != VC_NULL) ? x : VC_X, 0),
        LLVMConstInt(intType, (y != VC_NULL) ? y : VC_Y, 0),
    };

    LLVMValueRef mask = LLVMConstVector(maskV, 2);

    return LLVMBuildShuffleVector(b->builder, v0, v1, mask, "shuf");
}

LLVMValueRef LSimdVector4Shuffle(struct Builder* b, LLVMValueRef v0, LLVMValueRef v1, LSimdVecC x, LSimdVecC y,
                                 LSimdVecC z, LSimdVecC w)
{
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMValueRef maskV[] = {
        LLVMConstInt(intType, (x != VC_NULL) ? x : VC_X, 0),
        LLVMConstInt(intType, (y != VC_NULL) ? y : VC_Y, 0),
        LLVMConstInt(intType, (z != VC_NULL) ? z : VC_Z, 0),
        LLVMConstInt(intType, (w != VC_NULL) ? w : VC_W, 0),
    };

    LLVMValueRef mask = LLVMConstVector(maskV, 4);

    return LLVMBuildShuffleVector(b->builder, v0, v1, mask, "shuf");
}

LLVMValueRef LSimdVector2HAdd(struct Builder* b, LLVMValueRef v)
{
    // Horizontal add on a 2-lane vector: x + y. The 4-lane shuffle trick
    // below cannot run on a <2 x float> — its mask lanes 2/3 would read the
    // second operand and the fadd operands would not type-match.
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMValueRef lo = LLVMBuildExtractElement(b->builder, v, LLVMConstInt(intType, 0, 0), "lo");
    LLVMValueRef hi = LLVMBuildExtractElement(b->builder, v, LLVMConstInt(intType, 1, 0), "hi");

    return LLVMBuildFAdd(b->builder, lo, hi, "hadd_scalar");
}

LLVMValueRef LSimdVector4HAdd(struct Builder* b, LLVMValueRef v)
{
    // To do a horizontal add, we need to first add pairwise (x + y, x + y, z + w, z + w)
    LLVMValueRef swap = LSimdVector4Shuffle(b, v, v, VC_Y, VC_X, VC_W, VC_Z);
    LLVMValueRef sum1 = LLVMBuildFAdd(b->builder, v, swap, "hadd1");

    // Extract lane 0 (x + y) and lane 2 (z + w) add as scalars
    LLVMValueRef lo
        = LLVMBuildExtractElement(b->builder, sum1, LLVMConstInt(LLVMInt32TypeInContext(b->ctx), 0, 0), "lo");

    LLVMValueRef hi
        = LLVMBuildExtractElement(b->builder, sum1, LLVMConstInt(LLVMInt32TypeInContext(b->ctx), 2, 0), "hi");

    return LLVMBuildFAdd(b->builder, lo, hi, "hadd_scalar");
}

LLVMValueRef LSimdVector3HAdd(struct Builder* b, LLVMValueRef v)
{
    // float3: x + y + z. Lane 3 is padding (a `.xyz` swizzle or a 0/0
    // division can leave any value there, NaN included) and must not leak
    // into the sum.
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMValueRef x = LLVMBuildExtractElement(b->builder, v, LLVMConstInt(intType, 0, 0), "x");
    LLVMValueRef y = LLVMBuildExtractElement(b->builder, v, LLVMConstInt(intType, 1, 0), "y");
    LLVMValueRef z = LLVMBuildExtractElement(b->builder, v, LLVMConstInt(intType, 2, 0), "z");

    return LLVMBuildFAdd(b->builder, LLVMBuildFAdd(b->builder, x, y, "hadd_xy"), z, "hadd_scalar");
}

LLVMValueRef LSimdVector2Broadcast(struct Builder* b, LLVMValueRef scalar)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 2);

    LLVMValueRef poisonVec = LLVMGetPoison(vecType);

    // Insert scalar into first lane
    LLVMValueRef singleComp
        = LLVMBuildInsertElement(b->builder, poisonVec, scalar, LLVMConstInt(intType, 0, 0), "vecinit");

    // Reorder to XXXX
    return LSimdVector2Shuffle(b, singleComp, poisonVec, VC_X, VC_X);
}

LLVMValueRef LSimdVector4Broadcast(struct Builder* b, LLVMValueRef scalar)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    LLVMValueRef poisonVec = LLVMGetPoison(vecType);

    // Insert scalar into first lane
    LLVMValueRef singleComp
        = LLVMBuildInsertElement(b->builder, poisonVec, scalar, LLVMConstInt(intType, 0, 0), "vecinit");

    // Reorder to XXXX
    return LSimdVector4Shuffle(b, singleComp, poisonVec, VC_X, VC_X, VC_X, VC_X);
}

LLVMValueRef LSimdVector2Construct(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    LLVMTypeRef vecType = LLVMVectorType(scalarType, 2);

    // Splat scalar to all lanes
    if (n->args.count == 1)
    {
        Value scalarValue = EmitExpr(b, (Node*)n->args.items[0]);
        return LSimdVector2Broadcast(b, scalarValue.value);
    }

    // Load float2
    else if (n->args.count == 2)
    {
        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        vec = LLVMBuildInsertElement(b->builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");

        return vec;
    }
    else
    {
        DiagErrorFmt(b->diag, n->base.range, "invalid call to vector2 construct");
    }

    return NULL;
}

#define IS_VECTOR(x_) (TypeDescIsSimdVector(&(x_).typeDesc))

#define IS_SCALAR(x_) (!TypeDescIsSimdVector(&(x_).typeDesc))
#define IS_ALL_SCALAR3(x_, y_, z_) (IS_SCALAR(x_) && IS_SCALAR(y_) && IS_SCALAR(z_))
#define IS_ALL_SCALAR4(x_, y_, z_, w_) (IS_SCALAR(x_) && IS_SCALAR(y_) && IS_SCALAR(z_) && IS_SCALAR(w_))

static LLVMValueRef Vector4Construct2Args(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);
    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    Value x = EmitExpr(b, (Node*)n->args.items[0]);
    Value y = EmitExpr(b, (Node*)n->args.items[1]);

    LLVMValueRef vec = LLVMGetPoison(vecType);
    LaneCount destLanes = IsNameSimdVector(n->callee);

    // float3(float2, float)
    if (destLanes == 3 && IS_VECTOR(x) && IS_SCALAR(y))
    {
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 0, 0), "vecext"),
            LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 1, 0), "vecext"),
            LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 2, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, LLVMConstReal(scalarType, 0.0), LLVMConstInt(intType, 3, 0),
                                     "vecinit");
        return vec;
    }

    // float4(float3, float)
    if (destLanes == 4 && IS_VECTOR(x) && IS_SCALAR(y))
    {
        for (unsigned i = 0; i < 3; i++)
        {
            vec = LLVMBuildInsertElement(
                b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, i, 0), "vecext"),
                LLVMConstInt(intType, i, 0), "vecinit");
        }
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 3, 0), "vecinit");
        return vec;
    }

    // float4(float2, float2)
    if (destLanes == 4 && IS_VECTOR(x) && IS_VECTOR(y))
    {
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 0, 0), "vecext"),
            LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 1, 0), "vecext"),
            LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, y.value, LLVMConstInt(intType, 0, 0), "vecext"),
            LLVMConstInt(intType, 2, 0), "vecinit");
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, y.value, LLVMConstInt(intType, 1, 0), "vecext"),
            LLVMConstInt(intType, 3, 0), "vecinit");
        return vec;
    }

    DiagErrorFmt(b->diag, n->base.range, "invalid call to vector construct");
    return NULL;
}

// Helper to construct a float4/float3 from 3 arguments
static LLVMValueRef Vector4Construct3Args(struct Builder* b, CallExpr* n)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);
    LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

    Value x = EmitExpr(b, (Node*)n->args.items[0]);
    Value y = EmitExpr(b, (Node*)n->args.items[1]);
    Value z = EmitExpr(b, (Node*)n->args.items[2]);

    LLVMValueRef vec = LLVMGetPoison(vecType);
    LaneCount destLanes = IsNameSimdVector(n->callee);

    // float3(1.0, 2.0, 3.0) all scalars, implicit w = 0.
    if (destLanes == 3 && IS_SCALAR(x) && IS_SCALAR(y) && IS_SCALAR(z))
    {
        vec = LLVMBuildInsertElement(b->builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, z.value, LLVMConstInt(intType, 2, 0), "vecinit");
        // Zero the final component
        vec = LLVMBuildInsertElement(b->builder, vec, LLVMConstReal(scalarType, 0.0), LLVMConstInt(intType, 3, 0),
                                     "vecinit");

        return vec;
    }

    // float4(float2(1.0, 2.0), 3.0, 4.0)
    if (destLanes == 4 && IS_VECTOR(x) && IS_SCALAR(y) && IS_SCALAR(z))
    {
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 0, 0), "vecext"),
            LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(
            b->builder, vec, LLVMBuildExtractElement(b->builder, x.value, LLVMConstInt(intType, 1, 0), "vecext"),
            LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 2, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, z.value, LLVMConstInt(intType, 3, 0), "vecinit");

        return vec;
    }

    DiagErrorFmt(b->diag, n->base.range, "invalid call to vector construct");
    return NULL;
}

LLVMValueRef LSimdVector4Construct(struct Builder* b, CallExpr* n)
{

    // Splat scalar to all lanes
    if (n->args.count == 1)
    {

        Value x = EmitExpr(b, (Node*)n->args.items[0]);

        // Already a vector, pass through (something like `float4(float4(...))`).
        // A float3 result keeps its padding lane zeroed.
        if (IS_VECTOR(x))
        {
            if (IsNameSimdVector(n->callee) == 3)
            {
                LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
                LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

                return LLVMBuildInsertElement(b->builder, x.value, LLVMConstReal(scalarType, 0.0),
                                              LLVMConstInt(intType, 3, 0), "vecinit");
            }

            return x.value;
        }

        // Broadcast the scalar value to all lanes
        LLVMValueRef vec = LSimdVector4Broadcast(b, x.value);

        // The output vector is going to be 3 lanes. Zero out the W component.
        if (IsNameSimdVector(n->callee) == 3)
        {
            LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
            LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

            vec = LLVMBuildInsertElement(b->builder, vec, LLVMConstReal(scalarType, 0.0), LLVMConstInt(intType, 3, 0),
                                         "vecinit");
        }

        return vec;
    }

    else if (n->args.count == 2)
    {
        return Vector4Construct2Args(b, n);
    }

    else if (n->args.count == 3)
    {
        return Vector4Construct3Args(b, n);
    }

    else if (n->args.count == 4)
    {
        LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
        LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);
        LLVMTypeRef vecType = LLVMVectorType(scalarType, 4);

        Value x = EmitExpr(b, (Node*)n->args.items[0]);
        Value y = EmitExpr(b, (Node*)n->args.items[1]);
        Value z = EmitExpr(b, (Node*)n->args.items[2]);
        Value w = EmitExpr(b, (Node*)n->args.items[3]);

        LLVMValueRef vec = LLVMGetPoison(vecType);

        vec = LLVMBuildInsertElement(b->builder, vec, x.value, LLVMConstInt(intType, 0, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, y.value, LLVMConstInt(intType, 1, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, z.value, LLVMConstInt(intType, 2, 0), "vecinit");
        vec = LLVMBuildInsertElement(b->builder, vec, w.value, LLVMConstInt(intType, 3, 0), "vecinit");

        return vec;
    }
    else
    {
        DiagErrorFmt(b->diag, n->base.range, "invalid call to vector construct");
    }

    return NULL;
}

LLVMValueRef LSimdVectorBinExpr(Builder* b, LLVMValueRef vec, LLVMValueRef rhs, const struct BinaryExpr* binexp)
{
    LLVMTypeRef scalarType = LLVMFloatTypeInContext(b->ctx);
    LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);

    unsigned laneCount = LLVMGetVectorSize(LLVMTypeOf(vec));
    LLVMTypeRef vecType = LLVMVectorType(scalarType, laneCount);

    // Broadcast rhs if it's a scalar, not a vector
    LLVMValueRef rhsVec = rhs;
    if (LLVMGetTypeKind(LLVMTypeOf(rhs)) != LLVMVectorTypeKind)
    {
        LLVMValueRef single
            = LLVMBuildInsertElement(b->builder, LLVMGetPoison(vecType), rhs, LLVMConstInt(intType, 0, 0), "splat");

        LLVMValueRef maskV[4];
        for (unsigned i = 0; i < laneCount; i++)
        {
            maskV[i] = LLVMConstInt(intType, i, 0);
        }
        LLVMValueRef mask = LLVMConstVector(maskV, laneCount);
        rhsVec = LLVMBuildShuffleVector(b->builder, single, single, mask, "shuf");
    }

    switch (binexp->op)
    {
    case BinAdd:
        return LLVMBuildFAdd(b->builder, vec, rhsVec, "");

    case BinSub:
        return LLVMBuildFSub(b->builder, vec, rhsVec, "");

    case BinMul:
        return LLVMBuildFMul(b->builder, vec, rhsVec, "");

    case BinDiv:
        return LLVMBuildFDiv(b->builder, vec, rhsVec, "");

    case BinBitOr:
    {
        // Bitcast LHS and RHS to int vectors, OR, then bitcast back to floats
        LLVMTypeRef intVecTy = LLVMVectorType(intType, laneCount);

        LLVMValueRef lhsInt = LLVMBuildBitCast(b->builder, vec, intVecTy, "or.lhs.bc");
        LLVMValueRef rhsInt = LLVMBuildBitCast(b->builder, rhsVec, intVecTy, "or.rhs.bc");
        LLVMValueRef orInt = LLVMBuildOr(b->builder, lhsInt, rhsInt, "or.tmp");
        return LLVMBuildBitCast(b->builder, orInt, vecType, "");
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
        DiagErrorFmt(b->diag, binexp->base.range, "unsupported operation for vector data type");
        break;
    }

    return NULL; // unreachable if op is valid
}

LLVMValueRef LSimdVectorDestructure(struct Builder* b, LLVMValueRef vec, const struct MemberExpr* expr)
{
    LSimdVecC c[4];
    int numComponents = BuildComponents(c, expr->member);

    if (numComponents == ER_MALFORMED)
    {
        DiagError(b->diag, expr->base.range, "malformed components in vector destructure");
        return NULL;
    }

    // Single lane extract
    if (numComponents == 1)
    {
        LSimdVecC index = c[0];

        // numComponents should be ER_MALFORMED or 0 if there are no valid components
        assert(index != VC_NULL);

        LLVMValueRef idx = LLVMConstInt(LLVMInt32TypeInContext(b->ctx), index, 0);
        return LLVMBuildExtractElement(b->builder, vec, idx, "vecext");
    }

    if (numComponents == 2)
    {
        return LSimdVector2Shuffle(b, vec, vec, c[0], c[1]);
    }

    if (numComponents == 3)
    {
        // A 3-component swizzle is a float3: take the padding lane from a
        // zero vector (shuffle index 4 = lane 0 of the second operand)
        // instead of carrying the source's w along.
        LLVMTypeRef intType = LLVMInt32TypeInContext(b->ctx);
        LLVMValueRef zeroVec = LLVMConstNull(LLVMVectorType(LLVMFloatTypeInContext(b->ctx), 4));
        LLVMValueRef maskV[] = {
            LLVMConstInt(intType, c[0], 0),
            LLVMConstInt(intType, c[1], 0),
            LLVMConstInt(intType, c[2], 0),
            LLVMConstInt(intType, 4, 0),
        };

        if (LLVMGetVectorSize(LLVMTypeOf(vec)) != 4)
        {
            // A float2 source (`v.xyx`): widen it through the generic 4-lane
            // shuffle, then zero the padding lane.
            LLVMValueRef wide = LSimdVector4Shuffle(b, vec, vec, c[0], c[1], c[2], VC_X);
            return LLVMBuildInsertElement(b->builder, wide, LLVMConstReal(LLVMFloatTypeInContext(b->ctx), 0.0),
                                          LLVMConstInt(intType, 3, 0), "swz.pad");
        }

        return LLVMBuildShuffleVector(b->builder, vec, zeroVec, LLVMConstVector(maskV, 4), "swz3");
    }

    if (numComponents == 4)
    {
        return LSimdVector4Shuffle(b, vec, vec, c[0], c[1], c[2], c[3]);
    }

    DiagError(b->diag, expr->base.range, "malformed components in vector destructure");
    return NULL;
}

// Extracts lane `i` of `v` as a scalar.
static LLVMValueRef SimdExtract(struct Builder* b, LLVMValueRef v, unsigned i)
{
    return LLVMBuildExtractElement(b->builder, v, LLVMConstInt(LLVMInt32TypeInContext(b->ctx), i, 0), "vecext");
}

// Builds an `N`-lane vector from `count` scalar components.
static LLVMValueRef SimdBuildVector(struct Builder* b, LLVMValueRef* comps, unsigned count)
{
    LLVMTypeRef vecType = LLVMVectorType(LLVMFloatTypeInContext(b->ctx), count);
    LLVMValueRef result = LLVMGetPoison(vecType);

    for (unsigned i = 0; i < count; i++)
    {
        result = LLVMBuildInsertElement(b->builder, result, comps[i],
                                        LLVMConstInt(LLVMInt32TypeInContext(b->ctx), i, 0), "vecins");
    }

    return result;
}

LLVMValueRef LSimdVectorDot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB, LaneCount lanes)
{
    LLVMTypeRef floatType = LLVMFloatTypeInContext(b->ctx);
    unsigned physLanes = LLVMGetVectorSize(LLVMTypeOf(vecA));

    if (lanes == 0 || lanes > (LaneCount)physLanes)
    {
        lanes = (LaneCount)physLanes;
    }

    // Element-wise product, then horizontal (lane-wise) sum of the logical
    // lanes only (float3's padding lane 3 is excluded).
    LLVMValueRef vecMul = LLVMBuildFMul(b->builder, vecA, vecB, "dot_mul");

    LLVMValueRef acc = LLVMConstReal(floatType, 0.0);

    for (LaneCount i = 0; i < lanes; i++)
    {
        acc = LLVMBuildFAdd(b->builder, acc, SimdExtract(b, vecMul, i), "dot_acc");
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

    // float2 cross is the scalar z-component: ax*by - ay*bx.
    if (lanes == 2)
    {
        return LLVMBuildFSub(b->builder, LLVMBuildFMul(b->builder, ax, by, ""), LLVMBuildFMul(b->builder, ay, bx, ""),
                             "cross_z");
    }

    LLVMValueRef az = SimdExtract(b, vecA, 2);
    LLVMValueRef bz = SimdExtract(b, vecB, 2);

    LLVMValueRef x = LLVMBuildFSub(b->builder, LLVMBuildFMul(b->builder, ay, bz, ""),
                                   LLVMBuildFMul(b->builder, az, by, ""), "cross_x");
    LLVMValueRef y = LLVMBuildFSub(b->builder, LLVMBuildFMul(b->builder, az, bx, ""),
                                   LLVMBuildFMul(b->builder, ax, bz, ""), "cross_y");
    LLVMValueRef z = LLVMBuildFSub(b->builder, LLVMBuildFMul(b->builder, ax, by, ""),
                                   LLVMBuildFMul(b->builder, ay, bx, ""), "cross_z");

    // float3 is stored as a 4-lane vector with lane 3 zeroed; float4 w = 0.
    LLVMValueRef zero = LLVMConstReal(LLVMFloatTypeInContext(b->ctx), 0.0);
    LLVMValueRef comps[4] = {x, y, z, zero};

    return SimdBuildVector(b, comps, 4);
}

LLVMValueRef LSimdVector3Dot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB)
{
    return LSimdVectorDot(b, vecA, vecB, 3);
}
