#pragma once

struct CEmitter;
struct Vec;

/**
 * @brief Emits the construction of a new SIMD vector
 */
void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args);
