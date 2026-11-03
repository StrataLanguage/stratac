#pragma once

#include <stddef.h>
#include <stdint.h>

// Named integer types. Each keeps the underlying type the code already used, so adopting one changes no
// behaviour; the name says what the number means.

// A compile-time integer constant: the folded value of an integer expression.
typedef int64_t ConstInt;

// The bit pattern of a compile-time integer constant, for unsigned and wrapping arithmetic.
typedef uint64_t ConstUInt;

// Element count of a `T[N]` array. Negative means a dynamic `T[]`.
typedef long ArrayLength;

// Logical lane count of a SIMD vector (2, 3 or 4), or 0 when unknown.
typedef int LaneCount;

// Width of an integer type in bits.
typedef unsigned BitWidth;

// Index of a field in an LLVM struct type (physical layout, after padding fields).
typedef unsigned FieldIndex;

// Number of entries on a scope, defer or owning-locals stack: a depth to unwind to.
typedef size_t StackMark;
