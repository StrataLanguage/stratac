#pragma once

#include <stdbool.h>
#define CSIMD_FALLBACK_VECTOR_NAME "__strata_float128"

struct CEmitter;
struct Vec;
struct BinaryExpr;
struct MemberExpr;

/**
 * @brief Emits the construction of a new SIMD vector
 */
void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args);
void CSimdVectorDestructure(struct CEmitter* emitter, const struct MemberExpr* expr, bool throughBox);
void CSimdVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp);
