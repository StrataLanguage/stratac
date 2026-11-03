/* Builder: SIMD vector builtins and lane lvalues. */
#include "Codegen/Builder/BuilderInternal.h"

/* A single-lane vector member lvalue (`v.x`): not memory-addressable — the
   vector value lives at `ptr` (a slot, array element, or box cell) and every
   write is load-vector / insert-lane / store-vector. Multi-component swizzle
   members return invalid: reads still work through the destructure path, but
   writes need a whole-vector rebuild. */
LValue MakeVectorLaneLValue(Builder* b, LValue none, const MemberExpr* m, LLVMValueRef vecAddr, TypeDesc vectorTd)
{
    const char* member = m->member;
    unsigned lane;

    if (strcmp(member, "x") == 0)
    {
        lane = 0;
    }
    else if (strcmp(member, "y") == 0)
    {
        lane = 1;
    }
    else if (strcmp(member, "z") == 0)
    {
        lane = 2;
    }
    else if (strcmp(member, "w") == 0)
    {
        lane = 3;
    }
    else
    {
        return none;
    }

    /* float2's LLVM vector has 2 lanes (float3/float4 share a 4-lane
       vector); an out-of-range lane would emit an invalid insert. */
    if (lane >= LLVMGetVectorSize(vectorTd.type))
    {
        return none;
    }

    none.valid = true;
    none.vectorLane = true;
    none.ptr = vecAddr;
    none.typeDesc = TypeDescMake(LLVMFloatTypeInContext(b->ctx), TDF_FLOAT, NULL);
    none.vectorType = vectorTd.type;
    none.lane = lane;

    return none;
}

Value EmitVectorConstruct(Builder* b, CallExpr* n)
{
    TypeName tn = MakeTypeName(b, n->callee);
    TypeDesc typeDesc = Resolve(b, &tn);

    LaneCount numVectorLanes = IsNameSimdVector(n->callee);

    Value v;

    /* Float2 */
    if (numVectorLanes == 2)
    {
        v.value = LSimdVector2Construct(b, n);
    }
    /* If we have a vector3 or vector4, treat it like a vector4 (float128). */
    else if (numVectorLanes >= 3)
    {
        v.value = LSimdVector4Construct(b, n);
    }

    if (!v.value)
    {
        v.value = LLVMGetPoison(typeDesc.type);
    }
    v.typeDesc = typeDesc;

    return v;
}

Value EmitVectorReduce(Builder* b, CallExpr* n)
{
    Value vA = EmitExpr(b, (Node*)VecGet(&n->args, 0));

    /* float2 is a 2-lane LLVM vector: the 4-lane horizontal-add trick would
       emit a shuffle whose mask walks off the vector. */
    if (LLVMGetVectorSize(LLVMTypeOf(vA.value)) == 2)
    {
        return ValueMake(LSimdVector2HAdd(b, vA.value), ResolveByName(b, "float"));
    }

    /* float3 rides in a 4-lane vector whose lane 3 is not part of the value
       (a swizzle/division may leave anything there, even NaN): sum x+y+z. */
    if (vA.typeDesc.simdLanes == 3)
    {
        return ValueMake(LSimdVector3HAdd(b, vA.value), ResolveByName(b, "float"));
    }

    return ValueMake(LSimdVector4HAdd(b, vA.value), ResolveByName(b, "float"));
}

Value EmitVectorDot(Builder* b, CallExpr* n)
{
    Value vA = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value vB = EmitExpr(b, (Node*)VecGet(&n->args, 1));

    /* Only the logical lanes contribute (float3 ignores lane 3). */
    LaneCount lanes = vA.typeDesc.simdLanes ? vA.typeDesc.simdLanes : vB.typeDesc.simdLanes;

    return ValueMake(LSimdVectorDot(b, vA.value, vB.value, lanes), ResolveByName(b, "float"));
}

Value EmitVectorCross(Builder* b, CallExpr* n)
{
    Value vA = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value vB = EmitExpr(b, (Node*)VecGet(&n->args, 1));

    LLVMValueRef value = LSimdVectorCross(b, vA.value, vB.value);

    TypeDesc resultTd;

    /* The cross product of float2 yields a scalar value */
    if (LLVMGetVectorSize(LLVMTypeOf(vA.value)) == 2)
    {
        resultTd = ResolveByName(b, "float");
    }
    else
    {
        /* The cross product of float3 and float4 return the same vector type as the arguments */
        resultTd = vA.typeDesc;
    }

    return ValueMake(value, resultTd);
}
