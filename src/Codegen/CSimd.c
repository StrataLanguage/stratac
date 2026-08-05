#include "Codegen/CSimd.h"
#include "Codegen/CBackend.h"
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

/*
 * Platform agnostic definitions
 */

void CSimdVectorConstruct(struct CEmitter* emitter, const struct Vec* args)
{
    EMIT_PLATFORMS(SSEVectorConstruct, NEONVectorConstruct, emitter, args);
}
