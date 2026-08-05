#pragma once

struct CEmitter;
struct Vec;
struct BinaryExpr;

/**
 * @brief Emits the construction of a new SIMD vector
 */
void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args);

void CSimdVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp);
