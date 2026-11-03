#pragma once
// SIMD vector lowering helpers for the LLVM backend.

#include "LLVMCApi.h"

#include "Core/Types.h"

typedef struct CallExpr CallExpr;
struct Builder;
struct BinaryExpr;
struct MemberExpr;

typedef enum LSimdVecC
{
    VC_NULL = -1,
    VC_X = 0,
    VC_Y,
    VC_Z,
    VC_W
} LSimdVecC;

LLVMValueRef LSimdVector2Shuffle(struct Builder* b, LLVMValueRef v0, LLVMValueRef v1, LSimdVecC x, LSimdVecC y);
LLVMValueRef LSimdVector4Shuffle(struct Builder* b, LLVMValueRef v0, LLVMValueRef v1, LSimdVecC x, LSimdVecC y,
                                 LSimdVecC z, LSimdVecC w);

LLVMValueRef LSimdVector2Broadcast(struct Builder* b, LLVMValueRef scalar);
// Broadcast `scalar` to all lanes of a returned vector.
LLVMValueRef LSimdVector4Broadcast(struct Builder* b, LLVMValueRef scalar);

LLVMValueRef LSimdVector2Construct(struct Builder* builder, CallExpr* n);
LLVMValueRef LSimdVector4Construct(struct Builder* builder, CallExpr* n);

LLVMValueRef LSimdVectorBinExpr(struct Builder* b, LLVMValueRef vec, LLVMValueRef rhs, const struct BinaryExpr* binexp);
LLVMValueRef LSimdVectorDestructure(struct Builder* b, LLVMValueRef vec, const struct MemberExpr* expr);

// Dot product of two SIMD vectors: scalar float result. Sums the first
// `lanes` lane products (0 = every LLVM lane; float3 passes 3).
LLVMValueRef LSimdVectorDot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB, LaneCount lanes);
// Cross product: float3/float4 -> a vector (float2 -> scalar z).
LLVMValueRef LSimdVectorCross(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB);
LLVMValueRef LSimdVector3Dot(struct Builder* b, LLVMValueRef vecA, LLVMValueRef vecB);

// Sum of all components of a float2 (horizontal add, aka reduce). Returns a scalar.
LLVMValueRef LSimdVector2HAdd(struct Builder* b, LLVMValueRef v);

// Sum of all components of a float4 (horizontal add, aka reduce). Returns a scalar.
LLVMValueRef LSimdVector4HAdd(struct Builder* b, LLVMValueRef v);

// Horizontal add of a float3 (lanes 0..2 of a 4-lane vector; lane 3 ignored).
LLVMValueRef LSimdVector3HAdd(struct Builder* b, LLVMValueRef v);
