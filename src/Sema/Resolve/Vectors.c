/* SIMD vector constructor and builtin resolution. */
#include "Sema/Resolve/ResolverInternal.h"

static bool SimdVector2ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float", "float"}, 2},
    };

    return CheckArgTypesUnordered(r, c, scope, valid, ARRAY_COUNT(valid));
}

static bool SimdVector3ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float3"},                  1}, /* float3(float3(1.0, 2.0, 3.0)) */
        {(const char*[]){"float2", "float"},         2}, /* float3(float2(1.0, 2.0), 3.0) */
        {(const char*[]){"float", "float", "float"}, 3}, /* float3(1.0, 2.0, 3.0) */
    };

    return CheckArgTypesUnordered(r, c, scope, valid, ARRAY_COUNT(valid));
}

static bool SimdVector4ConstructValidate(Resolver* r, CallExpr* c, StrMap* scope)
{
    const AllowList valid[] = {
        {(const char*[]){"float4"},                           1}, /* float4(float4(1.0, 2.0, 3.0, 4.0)) */
        {(const char*[]){"float3", "float"},                  2}, /* float4(float3(1.0, 2.0, 3.0), 4.0) */
        {(const char*[]){"float2", "float", "float"},         3}, /* float4(float2(1.0, 2.0), 3.0, 4.0) */
        {(const char*[]){"float2", "float2"},                 2}, /* float4(float2(1.0, 2.0), float2(3.0, 4.0)) */
        {(const char*[]){"float", "float", "float", "float"}, 4}, /* float4(1.0, 2.0, 3.0, 4.0) */
    };

    return CheckArgTypesUnordered(r, c, scope, valid, ARRAY_COUNT(valid));
}

/* Resolves SIMD vector constructors (`float3(x)`, `float4(w,x,y,z)`); returns true if this was one. */
bool ResolveVectorConstruct(Resolver* r, CallExpr* c, StrMap* scope)
{
    // The function name is the same as the type name, so we can use TypeUtil functions on it.
    LaneCount numLanes = IsNameSimdVector(c->callee);
    if (numLanes == 0)
    {
        return false;
    }

    bool isValidCall = false;

    if (numLanes == 2)
    {
        isValidCall = SimdVector2ConstructValidate(r, c, scope);
    }
    else if (numLanes == 3)
    {
        isValidCall = SimdVector3ConstructValidate(r, c, scope);
    }
    else if (numLanes == 4)
    {
        isValidCall = SimdVector4ConstructValidate(r, c, scope);
    }

    if (!isValidCall)
    {
        DiagErrorFmt(r->diag, c->base.range, "no matching call to '%s' with %zu arguments", c->callee, c->args.count);
        return false;
    }

    c->isIntrinsicCall = true;

    return true;
}

//-- SIMD vector intrinsics.
bool ResolveVector2Arg(Resolver* r, CallExpr* c, StrMap* scope)
{
    // User defined functions take precedence
    if (ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
    {
        return false;
    }

    if (c->args.count != 2)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' expects 2 arguments, got %zu", c->callee, c->args.count);
        return true;
    }

    Node* a0 = (Node*)VecGet(&c->args, 0);
    Node* a1 = (Node*)VecGet(&c->args, 1);

    const TypeName* t0 = InferType(r, a0, scope);
    const TypeName* t1 = InferType(r, a1, scope);

    const char* resolved0 = t0 ? TypeRegistryResolveAlias(&r->registry, t0->name) : "";
    const char* resolved1 = t1 ? TypeRegistryResolveAlias(&r->registry, t1->name) : "";

    LaneCount lanes0 = IsNameSimdVector(resolved0);
    LaneCount lanes1 = IsNameSimdVector(resolved1);

    if (lanes0 == 0 || lanes1 == 0)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' expects SIMD vector arguments (float2/float3/float4), not '%s'",
                     c->callee, t0 ? t0->name : "?");

        return true;
    }

    if (lanes0 != lanes1)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' requires both vectors to have the same lane count ('%s' vs '%s')",
                     c->callee, t0->name, t1->name);

        return true;
    }

    return true;
}

bool ResolveVector1Arg(Resolver* r, CallExpr* c, StrMap* scope)
{
    // User defined functions take precedence
    if (ModuleHasFunctionNamed(r, c->callee, c->base.range.fileId))
    {
        return false;
    }

    if (c->args.count != 1)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' expects 1 argument, got %zu", c->callee, c->args.count);
        return true;
    }

    Node* vA = (Node*)VecGet(&c->args, 0);
    const TypeName* vAType = InferType(r, vA, scope);

    const char* resolved0 = vAType ? TypeRegistryResolveAlias(&r->registry, vAType->name) : "";

    LaneCount lanes = IsNameSimdVector(resolved0);

    if (lanes == 0)
    {
        DiagErrorFmt(r->diag, c->base.range, "'%s' expects SIMD vector arguments (float2/float3/float4), not '%s'",
                     c->callee, vAType ? vAType->name : "?");

        return true;
    }

    return true;
}

const TypeName* VectorConstructBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    if (IsNameSimdVector(c->callee) != 0)
    {
        return InternTypeName(r, c->callee);
    }

    return NULL;
}

const TypeName* VectorDotBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    return InternTypeName(r, "float");
}

const TypeName* VectorCrossBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    Node* a0 = (Node*)VecGet(&c->args, 0);
    const TypeName* a0Type = InferType(r, a0, scope);

    const char* resolved = TypeRegistryResolveAlias(&r->registry, a0Type ? a0Type->name : "");
    if (GetNameSimdVectorLanes(resolved) == 2)
    {
        return InternTypeName(r, "float");
    }

    return a0Type;
}

const TypeName* VectorReduceBuiltinType(Resolver* r, CallExpr* c, StrMap* scope)
{
    return InternTypeName(r, "float");
}
