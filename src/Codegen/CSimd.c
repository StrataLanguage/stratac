#include "Codegen/CSimd.h"
#include "Codegen/CBackend.h"
#include <AST/AST.h>
#include <Core/Util.h>

#include <strata/strata.h>

/* Emit platform specific code */
#define EMIT_PLATFORMS(x64_def_, arm64_def_, ...)                                                                      \
    switch (emitter->arch)                                                                                             \
    {                                                                                                                  \
    case STRATA_ARCH_X64:                                                                                              \
        x64_def_(__VA_ARGS__);                                                                                         \
        break;                                                                                                         \
    case STRATA_ARCH_ARM64:                                                                                            \
        arm64_def_(__VA_ARGS__);                                                                                       \
        break;                                                                                                         \
    default:;                                                                                                          \
    }

/*
 * NEON definitions
 */

static inline void NEONVectorConstruct(CEmitter* emitter, const Vec* args)
{
    /* Single scalar / splat */
    if (args->count == 1)
    {
        /* vdupq_n_f32 ( [scalar] ); */

        SbPuts(&emitter->out, "vdupq_n_f32(");
        CEmitExpr(emitter, args->items[0]);
        SbPuts(&emitter->out, ")");
    }
    else
    {
        /* This currently uses the C99 syntax for loading into a vector. This is mainly since we can't see into
           the future with nodes, so we cannot output a const array of values before an assignment or return.
           Since Neon doesn't have a _mm_setr adjacent function, this is our only option for now. */

        SbPuts(&emitter->out, "(float32x4_t) {");

        for (int i = 0; i < args->count; i++)
        {
            CEmitExpr(emitter, args->items[i]);
            if (i < args->count - 1)
            {
                SbPutc(&emitter->out, ',');
            }
        }

        /* If this is not padded out (only 3 components) then add a padding zero */
        if ((args->count % 4) != 0)
        {
            SbPuts(&emitter->out, ", 0.0000f");
        }

        SbPuts(&emitter->out, "}");
    }
}

static inline void NEONEmitExprAndExtend(CEmitter* emitter, Node* node)
{
    if (node->kind == NodeFloatLiteral)
    {
        SbPuts(&emitter->out, "vdupq_n_f32(");
        CEmitExpr(emitter, node);
        SbPutc(&emitter->out, ')');
    }
    else
    {
        CEmitExpr(emitter, node);
    }
}

static inline void NEONEmitArithOp(const char* intrin, struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    SbPuts(&emitter->out, intrin);
    SbPutc(&emitter->out, '(');
    NEONEmitExprAndExtend(emitter, binexp->lhs);
    SbPutc(&emitter->out, ',');
    NEONEmitExprAndExtend(emitter, binexp->rhs);
    SbPutc(&emitter->out, ')');
}

static inline void NEONEmitBitwiseOp(const char* intrin, struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    // vandq_u32 ( vreinterpretq_u32_f32(a) , vreinterpretq_u32_f32(b) );

    SbPuts(&emitter->out, "vreinterpretq_f32_u32(");
    SbPuts(&emitter->out, intrin);
    SbPuts(&emitter->out, "(vreinterpretq_u32_f32(");
    NEONEmitExprAndExtend(emitter, binexp->lhs);
    SbPuts(&emitter->out, "), vreinterpretq_u32_f32(");
    NEONEmitExprAndExtend(emitter, binexp->rhs);
    SbPuts(&emitter->out, ")))");
}

static inline void NEONVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    switch (binexp->op)
    {
    case BinAdd:
        NEONEmitArithOp("vaddq_f32", emitter, binexp);
        break;
    case BinSub:
        NEONEmitArithOp("vsubq_f32", emitter, binexp);
        break;
    case BinMul:
        NEONEmitArithOp("vmulq_f32", emitter, binexp);
        break;
    case BinDiv:
        NEONEmitArithOp("vdivq_f32", emitter, binexp);
        break;
    case BinBitAnd:
        /* Note that _u32 suffix here is due to NEON requiring uint vectors for bitwise operations. reinterpreting is
           handled by NEONEmitBitwiseOp. */
        NEONEmitBitwiseOp("vandq_u32", emitter, binexp);
        break;
    case BinBitOr:
        NEONEmitBitwiseOp("vorrq_u32", emitter, binexp);
        break;
    case BinBitXor:
        NEONEmitBitwiseOp("veorq_u32", emitter, binexp);
        break;
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
        DiagError(emitter->diag, binexp->base.range, "Unsupported expression for vector data type");
        break;
        break;
    default:;
    }
}

/*
 * SSE definitions
 */

static inline void SSEVectorConstruct(CEmitter* emitter, const Vec* args)
{
    /* Single scalar / splat */
    if (args->count == 1)
    {
        /* _mm_set1_ps ( [scalar] );  */

        SbPuts(&emitter->out, "_mm_set1_ps(");
        CEmitExpr(emitter, args->items[0]);
        SbPuts(&emitter->out, ")");
    }
    else
    {
        /* This currently uses the C99 syntax for loading into a vector. This is mainly since we can't see into
           the future with nodes, so we cannot output a const array of values before an assignment or return.
           Since Neon doesn't have a _mm_setr adjacent function, this is our only option for now. */

        SbPuts(&emitter->out, "_mm_setr_ps(");

        for (int i = 0; i < args->count; i++)
        {
            CEmitExpr(emitter, args->items[i]);
            if (i < args->count - 1)
            {
                SbPutc(&emitter->out, ',');
            }
        }

        /* If this is not padded out (only 3 components) then add a padding zero */
        if ((args->count % 4) != 0)
        {
            SbPuts(&emitter->out, ", 0.0000f");
        }

        SbPuts(&emitter->out, ")");
    }
}

static inline void SSEEmitExprAndExtend(CEmitter* emitter, Node* node)
{
    if (node->kind == NodeFloatLiteral)
    {
        SbPuts(&emitter->out, "_mm_set1_ps(");
        CEmitExpr(emitter, node);
        SbPutc(&emitter->out, ')');
    }
    else
    {
        CEmitExpr(emitter, node);
    }
}

static inline void SSEEmitArithOp(const char* intrin, struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    SbPuts(&emitter->out, intrin);
    SbPutc(&emitter->out, '(');
    SSEEmitExprAndExtend(emitter, binexp->lhs);
    SbPutc(&emitter->out, ',');
    SSEEmitExprAndExtend(emitter, binexp->rhs);
    SbPutc(&emitter->out, ')');
}

static inline void SSEVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    switch (binexp->op)
    {
    case BinAdd:
        SSEEmitArithOp("_mm_add_ps", emitter, binexp);
        break;
    case BinSub:
        SSEEmitArithOp("_mm_sub_ps", emitter, binexp);
        break;
    case BinMul:
        SSEEmitArithOp("_mm_mul_ps", emitter, binexp);
        break;
    case BinDiv:
        SSEEmitArithOp("_mm_div_ps", emitter, binexp);
        break;
    case BinBitAnd:
        SSEEmitArithOp("_mm_and_ps", emitter, binexp);
        break;
    case BinBitOr:
        SSEEmitArithOp("_mm_or_ps", emitter, binexp);
        break;
    case BinBitXor:
        SSEEmitArithOp("_mm_xor_ps", emitter, binexp);
        break;
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
        DiagError(emitter->diag, binexp->base.range, "Unsupported expression for vector data type");
        break;
        break;
    default:;
    }
}

/*
 * Platform agnostic definitions
 */

void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args)
{
    EMIT_PLATFORMS(SSEVectorConstruct, NEONVectorConstruct, emitter, args);
}

void CSimdVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    EMIT_PLATFORMS(SSEVectorBinExpr, NEONVectorBinExpr, emitter, binexp);
}
