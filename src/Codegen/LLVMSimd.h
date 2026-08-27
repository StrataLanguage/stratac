#pragma once

#include "LLVMCApi.h"

typedef struct CallExpr CallExpr;
struct Builder;
struct BinaryExpr;
struct MemberExpr;

/* Broadcast `scalar` to all lanes of a returned vector. */
LLVMValueRef LSimdVectorBroadcast(struct Builder* b, LLVMValueRef scalar);
LLVMValueRef LSimdVectorConstruct(struct Builder* builder, CallExpr* n);
LLVMValueRef LSimdVectorBinExpr(struct Builder* b, LLVMValueRef vec, LLVMValueRef rhs, const struct BinaryExpr* binexp);
LLVMValueRef LSimdVectorDestructure(struct Builder* b, LLVMValueRef vec, const struct MemberExpr* expr);
