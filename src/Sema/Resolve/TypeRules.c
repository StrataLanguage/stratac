/* Type predicates, interning, mangling and assignability rules. */
#include "Sema/Resolve/ResolverInternal.h"

bool IsBoolType(const TypeRegistry* reg, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    return GetPrimitiveType(TypeRegistryResolveAlias(reg, t->name)) == PrimBool;
}

/* Types a boolean context (`if`/`while`/`for`, `!`, `&&`, `||`) accepts:
   `bool`, `^bool` (reads through), and any `T?` (truthiness is presence).
   Numerics are not: `if (x)` on a float is a hidden != 0.0 test. */
bool IsBoolContextType(const TypeRegistry* reg, const TypeName* t)
{
    if (t && TypeNameIsOptional(t))
    {
        return true;
    }

    return IsBoolType(reg, t && TypeNameIsBox(t) && t->inner ? t->inner : t);
}

/* Scalars convert implicitly among themselves, except across the bool
   boundary: `bool b = 0.5` (a != 0 test) or `int n = flag` is almost always
   a bug, so numeric <-> bool needs an explicit cast. */
bool IsImplicitScalarConversion(PrimitiveType target, PrimitiveType value)
{
    return IsNumeric(target) && IsNumeric(value) && (target == PrimBool) == (value == PrimBool);
}

/* Intern a TypeName tree per canonical spelling (shared by synthesized types). */
const TypeName* InternTypeName(Resolver* r, const char* name)
{
    const TypeName* cached = (const TypeName*)StrMapGet(&r->typeCache, name);

    if (cached)
    {
        return cached;
    }

    TypeName* t = (TypeName*)ArenaAlloc(r->arena, sizeof(TypeName));
    *t = TypeNameParse(r->arena, name);
    StrMapPut(&r->typeCache, t->name, t);

    return t;
}

char* Mangle(Arena* arena, const FunctionDecl* f)
{
    Sb sb;
    SbInit(&sb);
    SbPuts(&sb, f->name);

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        SbPutc(&sb, '$');
        SbPuts(&sb, p->type.name);

        /* Distinguish a variadic tail (T...) from a plain T[] param. */
        if (p->isVarargRest)
        {
            SbPuts(&sb, "...");
        }
    }

    return SbFinish(&sb, arena);
}

bool IsDefinedStruct(const TypeRegistry* reg, const char* name)
{
    return TypeRegistryIsUserType(reg, name) && !TypeRegistryIsOpaque(reg, name);
}

bool IsIncompleteStruct(const TypeRegistry* reg, const char* name)
{
    const StructType* t = TypeRegistryFind(reg, name);
    return t && t->opaque && t->incomplete;
}

/* Strips a leading optional then box, returning the inner type (or NULL). */
const TypeName* UnwrapBoxPtr(const TypeName* t)
{
    if (!t)
    {
        return NULL;
    }

    if (TypeNameIsOptional(t))
    {
        t = t->inner;
    }

    if (t && TypeNameIsBox(t))
    {
        t = t->inner;
    }

    return t;
}

/* Owning when the type itself owns, or its alias resolves to one. */
bool AliasIsOwning(const Resolver* r, const TypeName* t)
{
    return TypeIsOwningResolved(&r->registry, r->arena, t);
}

/* Owning VALUE: AliasIsOwning plus owning structs (and their aliases) — a
   `Rec` value owns its fields, so it moves like a box when consumed and
   legalizes `Rec[]` as inline owning storage. */
bool AliasIsOwningValue(const Resolver* r, const TypeName* t)
{
    return TypeIsOwningValueResolved(&r->registry, r->arena, t);
}

/* True when the type is a dynamic array, or its alias resolves to one. */
bool ResolvesToDynamicArray(const Resolver* r, const TypeName* t)
{
    if (!t || !t->name)
    {
        return false;
    }

    if (TypeNameIsDynamicArray(t))
    {
        return true;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->registry, t->name);

    if (!leaf || strcmp(leaf, t->name) == 0)
    {
        return false;
    }

    TypeName parsed = TypeNameParse(r->arena, leaf);
    return TypeNameIsDynamicArray(&parsed);
}

/* True when both names resolve to the same underlying type.
   Distinct aliases never unify implicitly; explicit casts use this. */
bool SameResolvedType(const Resolver* r, const char* a, const char* b)
{
    const char* la = a ? TypeRegistryResolveAlias(&r->registry, a) : NULL;
    const char* lb = b ? TypeRegistryResolveAlias(&r->registry, b) : NULL;

    return la && lb && strcmp(la, lb) == 0;
}

//-- Conditional expressions.
static int NumericRank(PrimitiveType p)
{
    switch (p)
    {
    case PrimDouble:
        return 9;
    case PrimFloat:
        return 8;
    case PrimULong:
        return 7;
    case PrimLong:
        return 6;
    case PrimUInt:
        return 5;
    case PrimInt:
        return 4;
    case PrimUShort:
        return 3;
    case PrimShort:
        return 2;
    case PrimByte:
        return 1;
    case PrimSByte:
        return 1;
    default:
        return 0;
    }
}

/* The type of `c ? a : b`: two string literals (or cstrings) give a borrowed `cstring`; otherwise the
   branch types must agree (or one convert to the other, numbers widening). Owning branches are
   rejected: one path's value would have to be dropped and the other's moved. */
const TypeName* ConditionalType(Resolver* r, ConditionalExpr* ce, StrMap* scope, bool diagnose)
{
    const TypeName* lt = InferType(r, ce->thenExpr, scope);
    const TypeName* rt = InferType(r, ce->elseExpr, scope);

    if (!lt || !rt)
    {
        return lt ? lt : rt;
    }

    bool lCs = ce->thenExpr->kind == NodeStrLiteral || TypeIsCString(&r->registry, lt->name);
    bool rCs = ce->elseExpr->kind == NodeStrLiteral || TypeIsCString(&r->registry, rt->name);

    if (lCs && rCs)
    {
        return InternTypeName(r, "cstring");
    }

    if (AliasIsOwning(r, lt) || AliasIsOwning(r, rt))
    {
        if (diagnose)
        {
            DiagErrorFmt(r->diag, ce->base.range, "'?:' branches cannot be owning values ('%s' and '%s'); use if/else",
                         lt->name, rt->name);
        }
        return lt;
    }

    if (strcmp(lt->name, rt->name) == 0)
    {
        return lt;
    }

    if (IsNumeric(lt->primitiveType) && IsNumeric(rt->primitiveType) && lt->primitiveType != PrimBool
        && rt->primitiveType != PrimBool)
    {
        return NumericRank(lt->primitiveType) >= NumericRank(rt->primitiveType) ? lt : rt;
    }

    if (IsAssignableType(r, lt, rt))
    {
        return lt;
    }

    if (IsAssignableType(r, rt, lt))
    {
        return rt;
    }

    if (diagnose)
    {
        DiagErrorFmt(r->diag, ce->base.range, "'?:' branches have incompatible types '%s' and '%s'", lt->name,
                     rt->name);
    }

    return lt;
}

//-- `@export` functions: the plain C ABI.

/* A value that crosses a C call in one register: a number, bool, enum, handle or cstring. */
bool IsCAbiScalar(Resolver* r, const TypeName* t)
{
    if (!t || !t->name || TypeNameIsOwning(t) || TypeNameHasArrayShape(t))
    {
        return false;
    }

    const char* leaf = TypeRegistryResolveAlias(&r->registry, t->name);

    if (IsHandleType(&r->registry, leaf) || TypeIsCString(&r->registry, leaf))
    {
        return true;
    }

    PrimitiveType prim = GetPrimitiveType(leaf);

    return prim != PrimNone && prim != PrimVoid && prim != PrimString && IsNameSimdVector(leaf) == 0;
}

// Bare `...` accepts scalars, strings, and handles (not structs/arrays).
static bool IsCVarargScalarish(Resolver* r, const TypeName* type)
{
    if (!type)
    {
        return false;
    }

    if (IsNumeric(type->primitiveType) || TypeIsString(&r->registry, type->name))
    {
        return true;
    }

    if (TypeNameHasArrayShape(type) || IsSimdVector(type->primitiveType))
    {
        return false;
    }

    if (TypeRegistryIsUserType(&r->registry, type->name))
    {
        return TypeRegistryIsOpaque(&r->registry, type->name);
    }

    return false;
}

// Bare `...` also accepts `^T` when T is scalar/string/handle.
bool IsCVarargCompatible(Resolver* r, const TypeName* type)
{
    if (!type || type->primitiveType == PrimVoid)
    {
        return false;
    }

    /* `cstring` crosses as `const char*` — always vararg-compatible. */
    if (TypeIsCString(&r->registry, type->name))
    {
        return true;
    }

    if (IsCVarargScalarish(r, type))
    {
        return true;
    }

    if (TypeNameIsOwning(type))
    {
        return IsCVarargScalarish(r, TypeNameBoxInner(type));
    }

    return false;
}

/**
 * @brief Checks to see if a vector is assignable to another vector type.
 */
bool IsVectorAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType,
                            const char* resolvedTarget, const char* resolvedValue)
{
    // Ethan's SIMD vector rules:
    //
    // - Identical vector types are of course valid.
    //
    // - Differing vector sizes cannot be implicitly converted. That would need to be explicitly cast or be build as the
    // destination vector type (e.g. `float2 y = value.xy`), to prevent unintended logic errors.
    //
    // - Source values that are numeric (but not vectors!) CAN be assigned to a vector type. They will be automatically
    // broadcasted to a vector of the destination's type. `bool` is not numeric here.

    const LaneCount destLanes = IsNameSimdVector(resolvedTarget);
    const LaneCount srcLanes = IsNameSimdVector(resolvedValue);

    // Check identical vector types
    if (destLanes && srcLanes && srcLanes == destLanes)
    {
        if (strcmp(targetType->name, valueType->name) != 0)
        {
            // They are differing types (e.g. int4 x = float4(1.0)).
            return false;
        }

        return true;
    }

    // Check vector dest and scalar numeric src
    if (destLanes && (srcLanes == 0 && IsNameNumeric(resolvedValue) && GetPrimitiveType(resolvedValue) != PrimBool))
    {
        // If the dest type is an alias, it is not assignable this way.
        if (TypeRegistryIsTypeAlias(&r->registry, targetType->name))
        {
            return false;
        }

        return true;
    }

    return false;
}

bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType)
{
    /* A `cstring` is a `const char*`: it passes wherever a raw `ptr` is expected. */
    if (targetType && valueType && TypeIsCString(&r->registry, valueType->name)
        && strcmp(targetType->name, STRATA_PTR_TYPE) == 0)
    {
        return true;
    }

    /* `cstring` copies into `string` (heap copy at the use site). The
       reverse is never implicit: only a string literal initializes a
       `cstring` (handled at each use site via IsStrLiteralForCString). */
    if (targetType && valueType && TypeIsCString(&r->registry, valueType->name)
        && TypeIsString(&r->registry, targetType->name))
    {
        return true;
    }

    if (targetType && TypeIsCString(&r->registry, targetType->name))
    {
        return valueType && SameResolvedType(r, valueType->name, targetType->name);
    }

    if (targetType && TypeNameIsOwning(targetType))
    {
        const TypeName* inner = TypeNameBoxInner(targetType);

        // `T?` and `^T` share an ABI; they convert when inners match.
        const TypeName* valueInner = TypeNameBoxInner(valueType);

        if (TypeNameIsOptional(targetType) && TypeNameIsBox(valueType) && inner && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        /* Smart cast: a `T?` goes where `^T` is wanted once flow typing
           proves it non-empty; CheckOptionalDeref demands the proof at each use site. */
        if (!TypeNameIsOptional(targetType) && TypeNameIsBox(targetType) && TypeNameIsOptional(valueType) && inner
            && valueInner)
        {
            return strcmp(inner->name, valueInner->name) == 0;
        }

        return (inner && strcmp(inner->name, valueType->name) == 0) || strcmp(valueType->name, targetType->name) == 0;
    }

    // Exact name match.
    if (strcmp(valueType->name, targetType->name) == 0)
    {
        return true;
    }

    // Numerics convert freely, bool excluded (TODO: require casts for lossy narrowing).
    if (IsImplicitScalarConversion(targetType->primitiveType, valueType->primitiveType))
    {
        return true;
    }

    const char* resolvedTarget = TypeRegistryResolveAlias(&r->registry, targetType->name);
    const char* resolvedValue = TypeRegistryResolveAlias(&r->registry, valueType->name);

    // Check SIMD vectors for assignability
    if (IsVectorAssignableType(r, targetType, valueType, resolvedTarget, resolvedValue))
    {
        return true;
    }

    const TypeName* valueInner = TypeNameBoxInner(valueType);

    return HandleExtendsFrom(&r->registry, valueType->name, targetType->name)
           || (valueInner && strcmp(valueInner->name, targetType->name) == 0);
}

/* Whether all-zero bytes are a valid `t`: what `{ {}; count }` and array_resize fill with. Empty
   strings, arrays and optionals and zeroed scalars are; a `^T` (which must point at
   something) is not, nor is a struct holding one. */
bool ZeroIsValid(Resolver* r, const TypeName* t, int depth)
{
    (void)depth;

    /* Classified once, aliases resolved: a `struct PB = ^P` alias is a box like the `^P` it names. */
    return !t || TypeClassify(&r->registry, r->arena, t).zeroValid;
}
