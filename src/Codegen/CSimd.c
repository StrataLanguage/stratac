#include "Codegen/CSimd.h"
#include "Codegen/CBackend.h"
#include <AST/AST.h>
#include <Core/Util.h>

#include <strata/strata.h>

#define INVALID_COMPONENTS -1

typedef enum : int
{
    VC_NULL = -1,
    VC_X = 0,
    VC_Y,
    VC_Z,
    VC_W
} VectorComponent;

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

#define EMIT_PLATFORMS2(def_, ...)                                                                                     \
    if ((emitter->emitFlags & CEmitEnableSIMD) == 0)                                                                   \
    {                                                                                                                  \
        None##def_(__VA_ARGS__);                                                                                       \
        return;                                                                                                        \
    }                                                                                                                  \
    switch (emitter->arch)                                                                                             \
    {                                                                                                                  \
    case STRATA_ARCH_X64:                                                                                              \
        SSE##def_(__VA_ARGS__);                                                                                        \
        break;                                                                                                         \
    case STRATA_ARCH_ARM64:                                                                                            \
        NEON##def_(__VA_ARGS__);                                                                                       \
        break;                                                                                                         \
    default:;                                                                                                          \
    }

static int GetComponentList(VectorComponent* buffer, const char* memberAccess)
{
    int count = 0;

    for (int i = 0; i < 4; i++)
    {
        buffer[i] = VC_NULL;

        char ch = memberAccess[i];
        if (ch == '\0')
        {
            break;
        }

        if (ch < 'w' || ch > 'z')
        {
            return VC_NULL;
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
        DiagError(emitter->diag, binexp->base.range, "unsupported expression for vector data type");
        break;
        break;
    default:;
    }
}

static inline void NEONVectorGetLane(struct CEmitter* emitter, const struct MemberExpr* expr, int lane)
{
    SbPuts(&emitter->out, "vgetq_lane_f32(");
    CEmitExpr(emitter, expr->base_node);
    SbPuts(&emitter->out, ", ");
    SbPutc(&emitter->out, (const char)lane + '0');
    SbPutc(&emitter->out, ')');
}

static inline void NEONVectorSplatLane(struct CEmitter* emitter, const struct MemberExpr* expr, int lane)
{
    SbPuts(&emitter->out, "vdupq_lane_f32(");
    CEmitExpr(emitter, expr->base_node);
    SbPuts(&emitter->out, ", ");
    SbPutc(&emitter->out, (const char)lane + '0');
    SbPutc(&emitter->out, ')');
}

#define MATCHCOMP3(c_, x_, y_, z_) ((c_)[0] == (x_) && (c_)[1] == (y_) && (c_)[2] == (z_))
#define MATCHCOMP4(c_, x_, y_, z_, w_) ((c_)[0] == (x_) && (c_)[1] == (y_) && (c_)[2] == (z_) && (c_)[3] == (w_))

static inline void NEONVectorDestructure(struct CEmitter* emitter, const struct MemberExpr* expr)
{
    VectorComponent c[4];

    int numComponents = GetComponentList(c, expr->member);

    if (numComponents < 1)
    {
        DiagError(emitter->diag, expr->base.range, "unknown components in vector destructure");
        return;
    }

    if (numComponents == 1)
    {
        switch (c[0])
        {
        case VC_X:
            NEONVectorGetLane(emitter, expr, 0);
            break;
        case VC_Y:
            NEONVectorGetLane(emitter, expr, 1);
            break;
        case VC_Z:
            NEONVectorGetLane(emitter, expr, 2);
            break;
        case VC_W:
            NEONVectorGetLane(emitter, expr, 3);
            break;

        case VC_NULL:
        default:;
        }

        return;
    }

    if (numComponents == 3)
    {
        /* vector.xyz */
        if (MATCHCOMP3(c, VC_X, VC_Y, VC_Z))
        {
            CEmitExpr(emitter, expr->base_node);
            return;
        }
        /* vector.xxx */
        if (MATCHCOMP3(c, VC_X, VC_X, VC_X))
        {
            NEONVectorSplatLane(emitter, expr, 0);
            return;
        }
        /* vector.yyy */
        if (MATCHCOMP3(c, VC_Y, VC_Y, VC_Y))
        {
            NEONVectorSplatLane(emitter, expr, 1);
            return;
        }
        /* vector.zzz */
        if (MATCHCOMP3(c, VC_Z, VC_Z, VC_Z))
        {
            NEONVectorSplatLane(emitter, expr, 2);
            return;
        }
        /* vector.www */
        if (MATCHCOMP3(c, VC_W, VC_W, VC_W))
        {
            NEONVectorSplatLane(emitter, expr, 3);
            return;
        }

        /*  TODO: add more specializations */

        /* Construct a new vector from the components. This is a bit of a messy fallback, but vtblx is out of the
           picture at the moment. */
        {
            /* (float32x4_t) { vgetq_lane_f32(vector, index_x), ... } */

            SbPuts(&emitter->out, "(float32x4_t) {");

            for (int i = 0; i < 4; i++)
            {

                SbPuts(&emitter->out, "vgetq_lane_f32(");
                CEmitExpr(emitter, expr->base_node);
                SbPutc(&emitter->out, ',');

                int laneIndex = (c[i] == VC_NULL) ? i : c[i];
                SbPutc(&emitter->out, '0' + laneIndex);

                SbPutc(&emitter->out, ')');

                if (i < 3)
                {
                    SbPutc(&emitter->out, ',');
                }
            }

            SbPuts(&emitter->out, "}");
            return;
        }
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
        DiagError(emitter->diag, binexp->base.range, "unsupported expression for vector data type");
        break;
    default:;
    }
}

static inline void SSEVectorEmitShuffleIndices(struct CEmitter* emitter, VectorComponent* c)
{
    SbPuts(&emitter->out, "_MM_SHUFFLE(");

    const int lanes = 4;

    for (int i = lanes - 1; i >= 0; i--)
    {
        /* If the swizzle index was not set (e.g. it should be unmodified in the final vector) then use the
           corresponding index from the vector. */

        int laneIndex = (c[i] == VC_NULL) ? i : c[i];

        SbPutc(&emitter->out, '0' + laneIndex);

        if (i > 0)
        {
            SbPutc(&emitter->out, ',');
        }
    }
    SbPutc(&emitter->out, ')');
}

static inline void SSEVectorDestructure(struct CEmitter* emitter, const struct MemberExpr* expr)
{
    VectorComponent c[4];

    int numComponents = GetComponentList(c, expr->member);

    if (numComponents == -1)
    {
        DiagError(emitter->diag, expr->base.range, "unknown components in vector destructure");
        return;
    }

    /* _mm_permute_ps( vector , _MM_SHUFFLE(ix, iy, iz, iw) ) */
    SbPuts(&emitter->out, "_mm_permute_ps(");
    CEmitExpr(emitter, expr->base_node);
    SbPutc(&emitter->out, ',');
    SSEVectorEmitShuffleIndices(emitter, c);
    SbPutc(&emitter->out, ')');
}

/*
 * No SIMD specializations
 */

static inline void NoneVectorConstruct(CEmitter* emitter, const Vec* args)
{
    /* Single scalar / splat */
    if (args->count == 1)
    {
        /* (__strata_float128) { [scalar] , [scalar] , [scalar] , [scalar] };  */

        SbPuts(&emitter->out, "(" CSIMD_FALLBACK_VECTOR_NAME "){");

        for (int i = 0; i < 4; i++)
        {
            CEmitExpr(emitter, args->items[0]);
            if (i < 3)
            {
                SbPutc(&emitter->out, ',');
            }
        }

        SbPuts(&emitter->out, "}");
    }
    else
    {
        SbPuts(&emitter->out, "(" CSIMD_FALLBACK_VECTOR_NAME "){");

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

static inline char NoneLaneToMember(int lane)
{
    switch (lane)
    {
    case 0:
        return 'x';
    case 1:
        return 'y';
    case 2:
        return 'z';
    case 3:
        return 'w';
    default:;
    }

    return 'x';
}

static inline void NoneEmitDirectLane(CEmitter* emitter, Node* node, int lane)
{
    if (node->kind == NodeFloatLiteral)
    {
        /* Emit the literal value */
        CEmitExpr(emitter, node);
    }
    else
    {
        /* Emit the member (e.g.  vector.x) */
        CEmitExpr(emitter, node);
        SbPutc(&emitter->out, '.');
        SbPutc(&emitter->out, NoneLaneToMember(lane));
    }
}

static inline void NoneEmitArithOp(const char* op, struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    SbPuts(&emitter->out, "(" CSIMD_FALLBACK_VECTOR_NAME ") {");

    for (int i = 0; i < 4; i++)
    {
        NoneEmitDirectLane(emitter, binexp->lhs, i);
        SbPuts(&emitter->out, op);
        NoneEmitDirectLane(emitter, binexp->rhs, i);

        if (i < 3)
        {
            SbPutc(&emitter->out, ',');
        }
    }

    SbPutc(&emitter->out, '}');
}

static inline void NoneVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    switch (binexp->op)
    {
    case BinAdd:
        NoneEmitArithOp("+", emitter, binexp);
        break;
    case BinSub:
        NoneEmitArithOp("-", emitter, binexp);
        break;
    case BinMul:
        NoneEmitArithOp("*", emitter, binexp);
        break;
    case BinDiv:
        NoneEmitArithOp("/", emitter, binexp);
        break;
    case BinBitAnd:
        NoneEmitArithOp("&", emitter, binexp);
        break;
    case BinBitOr:
        NoneEmitArithOp("|", emitter, binexp);
        break;
    case BinBitXor:
        NoneEmitArithOp("^", emitter, binexp);
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
        DiagError(emitter->diag, binexp->base.range, "unsupported expression for vector data type");
        break;
    default:;
    }
}

void NoneVectorDestructure(struct CEmitter* emitter, const struct MemberExpr* expr)
{
    VectorComponent c[4];

    int numComponents = GetComponentList(c, expr->member);

    if (numComponents == -1)
    {
        DiagError(emitter->diag, expr->base.range, "unknown components in vector destructure");
        return;
    }

    SbPuts(&emitter->out, "(" CSIMD_FALLBACK_VECTOR_NAME ") {");

    for (int i = 0; i < 4; i++)
    {

        CEmitExpr(emitter, expr->base_node);
        SbPutc(&emitter->out, '.');

        int laneIndex = (c[i] == VC_NULL) ? i : c[i];
        SbPutc(&emitter->out, NoneLaneToMember(laneIndex));

        if (i < 3)
        {
            SbPutc(&emitter->out, ',');
        }
    }

    SbPuts(&emitter->out, "}");
}

/*
 * Platform agnostic definitions
 */

void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args)
{
    EMIT_PLATFORMS2(VectorConstruct, emitter, args);
}

void CSimdVectorBinExpr(struct CEmitter* emitter, const struct BinaryExpr* binexp)
{
    EMIT_PLATFORMS2(VectorBinExpr, emitter, binexp);
}

void CSimdVectorDestructure(struct CEmitter* emitter, const struct MemberExpr* expr)
{
    EMIT_PLATFORMS2(VectorDestructure, emitter, expr);
}
