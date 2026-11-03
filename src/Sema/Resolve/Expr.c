/* Expression type inference and expression resolution. */
#include "Sema/Resolve/ResolverInternal.h"

static const TypeName* InferTypeImpl(Resolver* r, Node* n, StrMap* scope);

/* Resets a map's keys without freeing its buffer (recycles per-function state). */
void ResetStrMap(StrMap* m)
{
    if (m->cap > 0)
    {
        memset(m->keys, 0, m->cap * sizeof(const char*));
        m->count = 0;
    }
}

int CountByName(const Module* mod, const char* name)
{
    int count = 0;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* functionDecl = (FunctionDecl*)VecGet(&mod->functions, i);

        if (strcmp(functionDecl->name, name) == 0)
        {
            count++;
        }
    }

    return count;
}

/* The name of the `const` binding (global, param or local) that the lvalue
   `target` is rooted at, or NULL when it is mutable. */
const char* ConstRootName(const Resolver* r, Node* target, StrMap* scope)
{
    Node* base = target;

    while (base && (base->kind == NodeMember || base->kind == NodeIndex || base->kind == NodeCast))
    {
        if (base->kind == NodeMember)
        {
            base = ((MemberExpr*)base)->base_node;
        }
        else if (base->kind == NodeIndex)
        {
            base = ((IndexExpr*)base)->base_node;
        }
        else
        {
            base = ((CastExpr*)base)->operand;
        }
    }

    if (!base || base->kind != NodeIdent)
    {
        return NULL;
    }

    const char* name = ((IdentExpr*)base)->name;
    const TypeName* declared = (const TypeName*)StrMapGet(scope, name);

    // Const locals are declared in `scope` (shadowing-aware); globals and params are in m_constVars.
    if ((declared && declared->isConst) || StrMapGet(&r->m_constVars, name))
    {
        return name;
    }

    return NULL;
}

static void CheckConstAssign(Resolver* r, Node* target, SourceRange range, StrMap* scope)
{
    if (!target)
    {
        return;
    }

    /* String and array's `.length` is readonly */
    if (target->kind == NodeMember)
    {
        MemberExpr* tm = (MemberExpr*)target;

        if (strcmp(tm->member, "length") == 0)
        {
            const TypeName* bt = InferType(r, tm->base_node, scope);

            if (bt && IsLengthPseudoMemberBase(&r->m_registry, bt))
            {
                DiagErrorFmt(r->m_diag, range,
                             "cannot assign to '%s' (a read-only pseudo-member; use 'array_resize' instead)",
                             tm->member);
                return;
            }
        }
    }

    const char* name = ConstRootName(r, target, scope);

    if (name)
    {
        DiagErrorFmt(r->m_diag, range, "'%s' is immutable", name);
    }
}

// True when every call arg matches a distinct allowed type.
bool CheckAllowListSingle(Resolver* r, CallExpr* c, StrMap* scope, const char** allowed, const int allowedSize)
{
    const int maxAllowed = 32;

    bool typesDepleted[32];
    memset(typesDepleted, 0, sizeof(typesDepleted));

    assert(allowedSize < maxAllowed);

    for (size_t i = 0; i < c->args.count; i++)
    {
        Node* arg = (Node*)VecGet(&c->args, i);

        const TypeName* argType = InferType(r, arg, scope);

        if (!argType)
        {
            continue;
        }

        bool isCorrectType = false;

        // Each allowed type may be used once.
        for (int j = 0; j < allowedSize; j++)
        {
            if (typesDepleted[j] == true)
            {
                continue;
            }

            if (strcmp(argType->name, allowed[j]) == 0)
            {

                isCorrectType = true;
                typesDepleted[j] = true;

                break;
            }
        }

        if (isCorrectType == false)
        {
            return false;
        }
    }

    return true;
}

const TypeName* InferType(Resolver* r, Node* n, StrMap* scope)
{
    const TypeName* t = InferTypeImpl(r, n, scope);
    NodeTypesPut(&r->m_nodeTypes, n, t);
    return t;
}

static const TypeName* InferTypeImpl(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    {
        const IntLiteral* lit = (const IntLiteral*)n;

        /* Values that don't fit a non-negative signed i32 are 64-bit, as is any `l` literal. */
        if (lit->value > 0x7FFFFFFFULL || lit->isLong)
        {
            return InternTypeName(r, lit->isUnsigned ? "ulong" : "long");
        }

        return InternTypeName(r, lit->isUnsigned ? "uint" : "int");
    }
    case NodeFloatLiteral:
        return InternTypeName(r, "float");
    case NodeBoolLiteral:
        return InternTypeName(r, "bool");
    case NodeStrLiteral:
        return InternTypeName(r, "string");
    case NodeIdent:
    {
        const TypeName* t = (const TypeName*)StrMapGet(scope, ((IdentExpr*)n)->name);

        /* The builtin `null` constant (a variable named `null` shadows it). */
        if (!t && IsNullConstantName(((IdentExpr*)n)->name))
        {
            return InternTypeName(r, STRATA_NULL_TYPE);
        }

        return t ? t : NULL;
    }
    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;

        return u->op == UnNot ? InternTypeName(r, "bool") : InferType(r, u->operand, scope);
    }
    case NodeBinary:
    {
        BinaryExpr* b = (BinaryExpr*)n;

        switch (b->op)
        {
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
        case BinLogicAnd:
        case BinLogicOr:
            return InternTypeName(r, "bool");
        default:
        {
            const TypeName* lt = InferType(r, b->lhs, scope);
            const TypeName* rt = InferType(r, b->rhs, scope);
            PrimitiveType lp = lt ? lt->primitiveType : PrimNone;

            if (IsEnumFlagsOp(r, b->op, lt, rt))
            {
                return lt;
            }

            if (IsStringConcat(r, b->op, lt, rt))
            {
                return InternTypeName(r, "string");
            }
            PrimitiveType rp = rt ? rt->primitiveType : PrimNone;

            /* Box/optional operands deref to their inner for arithmetic
               (mirrors the ResolveExpr operand check). Same-shape SIMD
               vectors keep the vector type: the scalar ladder below would
               report a bogus `int` and break overload matching for valid
               vector arguments. */
            const char* ln2 = lt && TypeNameIsBox(lt) && lt->inner ? lt->inner->name : lt ? lt->name : "";
            const char* rn2 = rt && TypeNameIsBox(rt) && rt->inner ? rt->inner->name : rt ? rt->name : "";

            if (SameResolvedType(r, ln2, rn2) && IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0)
            {
                return InternTypeName(r, TypeRegistryResolveAlias(&r->m_registry, ln2));
            }

            if (lp == PrimDouble || rp == PrimDouble)
            {
                return InternTypeName(r, "double");
            }

            if (lp == PrimFloat || rp == PrimFloat)
            {
                return InternTypeName(r, "float");
            }

            if (lp == PrimULong || rp == PrimULong)
            {
                return InternTypeName(r, "ulong");
            }

            if (lp == PrimLong || rp == PrimLong)
            {
                return InternTypeName(r, "long");
            }

            if (lp == PrimUInt || rp == PrimUInt)
            {
                return InternTypeName(r, "uint");
            }

            return InternTypeName(r, "int");
        }
        }
    }
    case NodeAssign:
        return InferType(r, ((AssignExpr*)n)->target, scope);
    case NodeNullTest:
        return InternTypeName(r, "bool");
    case NodeConditional:
    {
        ConditionalExpr* ce = (ConditionalExpr*)n;

        return ce->resultType ? ce->resultType : ConditionalType(r, ce, scope, false);
    }
    case NodeIncDec:
        return InferType(r, ((IncDecExpr*)n)->operand, scope);
    case NodeCast:
        return &((CastExpr*)n)->type;
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;

        /* Scoped enum constant: `EnumName.Member` has the enum's type. */
        if (m->isEnumConst && m->enumTypeName)
        {
            return InternTypeName(r, m->enumTypeName);
        }

        /* Builtin scalar pseudo-property: `int.max` / `float.min` has the
           base scalar's own type. Resolution-order independent (like the
           impl-property path below). */
        if (m->base_node->kind == NodeIdent)
        {
            const char* baseName = ((IdentExpr*)m->base_node)->name;
            PrimitiveType primitive = GetPrimitiveType(baseName);

            if (IsScalarPseudoType(primitive) && ScalarPseudoConst(primitive, m->member, NULL, NULL, NULL))
            {
                return InternTypeName(r, baseName);
            }
        }

        const TypeName* baseType = InferType(r, m->base_node, scope);

        /* Impl properties: a member read on a handle with a matching
           `property` yields the property's type. Methods are call-only
           (NULL here; the resolve pass diagnoses bare access). */
        PropertyDecl* prop = PropertyOnHandle(r, baseType, m->member);

        if (prop)
        {
            return &prop->returnType;
        }

        /* Optional member access follows the box pointer. */
        baseType = UnwrapBoxPtr(baseType);

        if (!baseType)
        {
            return NULL;
        }

        /* SIMD lane read / swizzle: `v.x` is a float, `v.xy` a float2, `v.xyz` a float3. */
        if (IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, baseType->name)) != 0)
        {
            size_t lanes = strlen(m->member);
            bool swizzle = lanes >= 1 && lanes <= 4;

            for (size_t i = 0; swizzle && i < lanes; i++)
            {
                swizzle = m->member[i] >= 'w' && m->member[i] <= 'z';
            }

            if (swizzle)
            {
                static const char* const swizzleTypes[5] = {NULL, "float", "float2", "float3", "float4"};
                return InternTypeName(r, swizzleTypes[lanes]);
            }
        }

        if (TypeRegistryIsOpaque(&r->m_registry, baseType->name))
        {
            if (IsIncompleteStruct(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of incomplete type '%s'", m->member,
                             baseType->name);
            }
            else
            {
                DiagErrorFmt(r->m_diag, m->base.range, "cannot access member '%s' of opaque handle '%s'", m->member,
                             baseType->name);
            }

            return NULL;
        }

        /* Array/string pseudo-properties: `.length` and `.cap` read the fat's
           u32 fields (uint). Fixed arrays expose a compile-time `.length`
           only — they have no capacity (inline storage). */
        if (IsFatPseudoProperty(r, baseType, m->member))
        {
            return InternTypeName(r, strcmp(m->member, "data") == 0 ? STRATA_PTR_TYPE : "uint");
        }

        const StructType* structType = TypeRegistryFind(&r->m_registry, baseType->name);
        if (!structType)
        {
            return NULL;
        }

        int idx = TypeRegistryFieldIndex(&r->m_registry, baseType->name, m->member);
        if (idx < 0)
        {
            return NULL;
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet((Vec*)&structType->fields, (size_t)idx);

        return &fieldDecl->type;
    }
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;

        if (c->resolvedDecl)
        {
            return &c->resolvedDecl->returnType;
        }

        // Infer the type for intrinsic calls
        const TypeName* intrinsicType = InferIntrinsicType(r, c, scope);
        if (intrinsicType != NULL)
        {
            return intrinsicType;
        }

        if (TypeRegistryIsUserType(&r->m_registry, c->callee))
        {
            return InternTypeName(r, c->callee);
        }

        return NULL;
    }
    case NodeStructInit:
        return ((StructInitExpr*)n)->typeName ? InternTypeName(r, ((StructInitExpr*)n)->typeName) : NULL;
    case NodeIndex:
    {
        const TypeName* baseType = InferType(r, ((IndexExpr*)n)->base_node, scope);

        /* Indexing through an optional array (`T[]?`) unwraps it. */
        if (baseType && TypeNameIsOptional(baseType))
        {
            baseType = baseType->inner;
        }

        if (baseType && TypeIsString(&r->m_registry, baseType->name))
        {
            // `s[i]` is a bounds-checked byte.
            return InternTypeName(r, "byte");
        }

        if (baseType && TypeIsCString(&r->m_registry, baseType->name))
        {
            // `cs[i]` is a bounds-checked byte read (strlen is the length).
            return InternTypeName(r, "byte");
        }

        return baseType ? TypeNameArrayElem(baseType) : NULL;
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* ai = (const ArrayInitExpr*)n;

        return ai->elementType ? InternTypeName(r, arena_format(r->m_arena, "%s[]", ai->elementType->name)) : NULL;
    }
    default:
        return NULL;
    }
}

/* `ref T x = path;` / `const ref T x = path;`: an alias of the storage at `path`, which must be a
   variable, field or element of type T (or a `^T` / proven `T?` holding one). Nothing is
   moved or copied; the alias cannot be rebound or moved from, and dies once
   what it refers to may have moved. */
void ResolveRefLocal(Resolver* r, VarDeclStmt* vd, StrMap* scope)
{
    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);

    if (!vd->init)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "ref local '%s' must be bound to a variable, field or element",
                     vd->name);
        return;
    }

    ResolveExpr(r, vd->init, scope);

    const char* key = MovableBoxSourceKey(r, vd->init);

    if (!IsLValueNode(vd->init) || !key)
    {
        DiagErrorFmt(r->m_diag, vd->init->range,
                     "ref local '%s' must be bound to a variable, field or element, not a temporary", vd->name);
        return;
    }

    const TypeName* initType = InferType(r, vd->init, scope);

    if (!initType)
    {
        return;
    }

    const TypeName* inner
        = (TypeNameIsBox(initType) || TypeNameIsOptional(initType)) ? TypeNameBoxInner(initType) : NULL;
    bool exact = SameResolvedType(r, initType->name, vd->type.name);
    bool through = !exact && !TypeNameIsBox(&vd->type) && !TypeNameIsOptional(&vd->type) && inner
                   && SameResolvedType(r, inner->name, vd->type.name);

    if (!exact && !through)
    {
        DiagErrorFmt(r->m_diag, vd->init->range, "'ref %s %s' cannot be bound to a '%s'", vd->type.name, vd->name,
                     initType->name);
        return;
    }

    if (through)
    {
        // Reaching into a `T?` reads through it: it must be proven set.
    }

    const char* constRoot = ConstRootName(r, vd->init, scope);

    if (constRoot && !vd->type.isConst)
    {
        DiagErrorFmt(r->m_diag, vd->init->range,
                     "'%s' is immutable, so a mutable ref cannot be bound into it; use 'const ref'", constRoot);
    }

    vd->refThroughBox = through;
    StrMapPut(&r->m_refLocals, vd->name, (void*)CanonicalRefKey(r, key));

    // Like a `ref` param: it does not own what it aliases, so it is never moved from or rebound.
    if (AliasIsOwning(r, &vd->type) || AliasIsOwningValue(r, &vd->type))
    {
        StrMapPut(&r->m_refBoxParams, vd->name, (void*)1);
    }
}

void ResolveExpr(Resolver* r, Node* n, StrMap* scope)
{
    ResolveExprImpl(r, n, scope, false);
}

static void ResolveIdentExpr(Resolver* r, Node* n, StrMap* scope)
{
    IdentExpr* ident = (IdentExpr*)n;
    const TypeName* varType = (const TypeName*)StrMapGet(scope, ident->name);

    if (!varType && IsNullConstantName(ident->name))
    {
        return;
    }

    if (!varType && strcmp(ident->name, "null") == 0)
    {
        DiagError(r->m_diag, ident->base.range,
                  "there is no 'null': a null pointer, handle or delegate is 'ptr.null', and an empty optional is "
                  "one never assigned");
    }
    else if (!varType)
    {
        DiagErrorFmt(r->m_diag, ident->base.range, "unknown variable '%s'", ident->name);
    }

    return;
}

static void ResolveUnaryExpr(Resolver* r, Node* n, StrMap* scope)
{
    UnaryExpr* u = (UnaryExpr*)n;

    ResolveExpr(r, u->operand, scope);

    /* `-bool` and `~bool` compute i1 garbage (signed 1-bit neg/not).
       Only `!` is defined for bools. */
    if (u->op == UnNeg || u->op == UnBitNot)
    {
        const TypeName* ot = InferType(r, u->operand, scope);

        if (ot && IsBoolType(&r->m_registry, ot))
        {
            DiagErrorFmt(r->m_diag, u->base.range, "invalid operand to unary operator ('%s')", ot->name);
        }
    }

    /* `~` complements the bits of a flags enum only. */
    if (u->op == UnBitNot)
    {
        const TypeName* ot = InferType(r, u->operand, scope);
        const char* en = ExprEnumName(r, u->operand, ot);

        if (en && IsPlainEnum(r, en))
        {
            DiagErrorFmt(r->m_diag, u->base.range, "'%s' is not a flags enum: declare it '@flags enum %s'", en, en);
        }
    }

    /* An enum value is a name, not a number to negate. */
    if (u->op == UnNeg)
    {
        const TypeName* ot = InferType(r, u->operand, scope);

        if (ot && TypeRegistryIsEnum(&r->m_registry, ot->name))
        {
            DiagErrorFmt(r->m_diag, u->base.range, "invalid operand to unary operator ('%s')", ot->name);
        }
    }

    // `!` is logical only: `!x` on a number is an implicit == 0 test.
    if (u->op == UnNot)
    {
        const TypeName* ot = InferType(r, u->operand, scope);

        if (ot && !IsBoolContextType(&r->m_registry, ot))
        {
            DiagErrorFmt(r->m_diag, u->base.range, "invalid operand to unary operator ('%s')", ot->name);
        }
    }

    return;
}

static void ResolveArithBinary(Resolver* r, BinaryExpr* b, StrMap* scope)
{
    const TypeName* lt = InferType(r, b->lhs, scope);
    const TypeName* rt = InferType(r, b->rhs, scope);
    const char* ln = lt ? lt->name : "";
    const char* rn = rt ? rt->name : "";

    // Unknown types and box/optional operands unwrap before checking.
    const TypeName* li = lt && TypeNameIsBox(lt) && lt->inner ? lt->inner : lt;
    const TypeName* ri = rt && TypeNameIsBox(rt) && rt->inner ? rt->inner : rt;
    const char* ln2 = li ? li->name : ln;
    const char* rn2 = ri ? ri->name : rn;

    bool numericPair = li && ri && IsNumeric(li->primitiveType) && IsNumeric(ri->primitiveType);
    bool vectorPair
        = SameResolvedType(r, ln2, rn2) && IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln2)) != 0;

    /* Bool is not numeric: `true + true` must not compute i1
       wrap-around (1+1 = 0). */
    bool boolOperand = (lt && IsBoolType(&r->m_registry, lt)) || (rt && IsBoolType(&r->m_registry, rt));

    /* `&`, `|`, `^` combine flags of one enum type (`Usage.A | Usage.B`). */
    bool flagsPair = IsEnumFlagsOp(r, b->op, lt, rt);

    /* `a + b` on strings / cstrings / literals concatenates into a new string. */
    bool concatPair = IsStringConcat(r, b->op, lt, rt);

    if (lt && rt && (!numericPair || boolOperand) && !vectorPair && !flagsPair && !concatPair)
    {
        DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
    }
    else if (numericPair)
    {
        CheckConstArith(r->m_diag, b, lt);
    }

    /* Bitwise operators combine the values of a `@flags` enum; any other enum is a set of names. */
    if (b->op == BinBitAnd || b->op == BinBitOr || b->op == BinBitXor)
    {
        const char* le = ExprEnumName(r, b->lhs, lt);
        const char* re = ExprEnumName(r, b->rhs, rt);
        const char* plain = le && IsPlainEnum(r, le) ? le : (re && IsPlainEnum(r, re) ? re : NULL);

        if (plain)
        {
            DiagErrorFmt(r->m_diag, b->base.range,
                         "'%s' is not a flags enum: declare it '@flags enum %s' to combine its values", plain, plain);
        }
    }
}

static void ResolveCompareBinary(Resolver* r, BinaryExpr* b, StrMap* scope)
{
    const TypeName* lt = InferType(r, b->lhs, scope);
    const TypeName* rt = InferType(r, b->rhs, scope);
    const char* ln = lt ? lt->name : "";
    const char* rn = rt ? rt->name : "";

    /* Two different enum types are never comparable, whatever their values. A constant such as
       `Color.Red` is typed as its underlying integer, so it is named by its member expression. */
    {
        const char* le = ExprEnumName(r, b->lhs, lt);
        const char* re = ExprEnumName(r, b->rhs, rt);

        if (le && re && strcmp(le, re) != 0)
        {
            DiagErrorFmt(r->m_diag, b->base.range, "cannot compare '%s' with '%s': they are different enums", le, re);
        }
    }

    bool lString = lt && TypeIsString(&r->m_registry, ln);
    bool rString = rt && TypeIsString(&r->m_registry, rn);
    bool lCStr = lt && TypeIsCString(&r->m_registry, ln);
    bool rCStr = rt && TypeIsCString(&r->m_registry, rn);

    /* A string literal on one side of a `cstring` comparison coerces
       to `cstring` (contextual), so `cs == "lit"` is well-typed. */
    if (lCStr && rString && b->rhs->kind == NodeStrLiteral)
    {
        rString = false;
        rCStr = true;
    }

    if (rCStr && lString && b->lhs->kind == NodeStrLiteral)
    {
        lString = false;
        lCStr = true;
    }

    /* Ordering a bool is meaningless (i1 compares with signed 1-bit
       semantics: `true < false` computes garbage). Only `==`/`!=`
       are defined for bools. */
    bool eqOp = (b->op == BinEqEq || b->op == BinNotEq);
    bool boolOperand = (lt && IsBoolType(&r->m_registry, lt)) || (rt && IsBoolType(&r->m_registry, rt));

    /* Comparing a `T?` reads through it. Only `==`/`!=` with an
       optional LEFT side and a box/optional right side is
       null-aware (empty == empty); every other comparison derefs the
       optional, so it must be proven non-empty first. `T[]?`
       compares as the array it wraps (empty is a zero-length fat). */
    {
        bool lOpt = lt && TypeNameIsOptional(lt) && !TypeIsComparableAggregate(&r->m_registry, lt);
        bool rOpt = rt && TypeNameIsOptional(rt) && !TypeIsComparableAggregate(&r->m_registry, rt);
        bool lNullAware = eqOp && lOpt && rt && (TypeNameIsOptional(rt) || TypeNameIsBox(rt));
        bool rNullAware = eqOp && lOpt;
    }

    /* `flag == 1` would compare through an implicit numeric <-> bool
       conversion; both sides of a bool comparison must be bool. */
    const TypeName* lScalar = TypeNameBoxInner(lt) ? TypeNameBoxInner(lt) : lt;
    const TypeName* rScalar = TypeNameBoxInner(rt) ? TypeNameBoxInner(rt) : rt;
    bool mixedBool = lScalar && rScalar && IsBoolType(&r->m_registry, lScalar) != IsBoolType(&r->m_registry, rScalar);

    if ((boolOperand && !eqOp) || mixedBool)
    {
        DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
        return;
    }

    /* `string == cstring` (either order) compares the text, like `string == string`. */
    if (eqOp && ((lString && rCStr) || (rString && lCStr)))
    {
        return;
    }

    if (lString != rString)
    {
        DiagErrorFmt(r->m_diag, b->base.range, "cannot compare 'string' with '%s'", lString ? rn : ln);
        return;
    }

    if (lString)
    {
        return;
    }

    /* `cstring` compares by content like `string`. */
    if (lCStr != rCStr)
    {
        DiagErrorFmt(r->m_diag, b->base.range, "cannot compare 'cstring' with '%s'", lCStr ? rn : ln);
        return;
    }

    if (lCStr)
    {
        if (!eqOp)
        {
            DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
        }

        return;
    }

    /* Aggregates (structs, dynamic/fixed arrays, `T[]?`): `==`/`!=`
       compare structurally — same resolved type only, never across
       distinct struct/array types or against scalars. Ordering is
       meaningless for aggregates and rejected. */
    if (lt && rt && (TypeIsComparableAggregate(&r->m_registry, lt) || TypeIsComparableAggregate(&r->m_registry, rt)))
    {
        bool eqOp = (b->op == BinEqEq || b->op == BinNotEq);

        if (!eqOp || !TypeIsComparableAggregate(&r->m_registry, lt) || !TypeIsComparableAggregate(&r->m_registry, rt)
            || !SameResolvedType(r, ln, rn))
        {
            DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
        }

        return;
    }

    if (lt && rt && (!TypeIsTriviallyComparable(&r->m_registry, lt) || !TypeIsTriviallyComparable(&r->m_registry, rt)))
    {
        DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
    }
    else if (lt && rt && !eqOp
             && (IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, ln)) != 0
                 || IsNameSimdVector(TypeRegistryResolveAlias(&r->m_registry, rn)) != 0))
    {
        /* Vectors are equal or not; they have no order. */
        DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", ln, rn);
    }
}

static void ResolveLogicBinary(Resolver* r, BinaryExpr* b, StrMap* scope)
{
    // Logical operators take bools only; no implicit numeric truthiness.
    const TypeName* lt = InferType(r, b->lhs, scope);
    const TypeName* rt = InferType(r, b->rhs, scope);

    if (lt && rt && (!IsBoolContextType(&r->m_registry, lt) || !IsBoolContextType(&r->m_registry, rt)))
    {
        DiagErrorFmt(r->m_diag, b->base.range, "invalid operands to binary operator ('%s' and '%s')", lt->name,
                     rt->name);
    }
}

static void ResolveBinaryExpr(Resolver* r, Node* n, StrMap* scope)
{
    BinaryExpr* b = (BinaryExpr*)n;

    ResolveExpr(r, b->lhs, scope);

    /* `a? && a.x`: the right side runs only when the left is true (`||`: when it is false). */
    if (b->op == BinLogicAnd || b->op == BinLogicOr)
    {
        ResolveExpr(r, b->rhs, scope);
    }
    else
    {
        ResolveExpr(r, b->rhs, scope);
    }

    // Arithmetic, comparison and logic operators each check their own operands.
    switch (b->op)
    {
    case BinAdd:
    case BinSub:
    case BinMul:
    case BinDiv:
    case BinMod:
    case BinBitAnd:
    case BinBitOr:
    case BinBitXor:
    case BinShl:
    case BinShr:
        ResolveArithBinary(r, b, scope);
        return;
    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
        ResolveCompareBinary(r, b, scope);
        return;
    case BinLogicAnd:
    case BinLogicOr:
        ResolveLogicBinary(r, b, scope);
        return;
    default:
        break;
    }
}

static bool CheckAssignTargetForm(Resolver* r, AssignExpr* a, StrMap* scope)
{
    /* Storing into a path frees or rebinds what was there: refs reaching through it die. */

    /* `cstring` is constant storage: element writes (plain or compound)
       are rejected. Whole-slot rebinding (`cs = ...`) is fine. */
    if (a->target->kind == NodeIndex)
    {
        Node* base = a->target;

        while (base && base->kind == NodeIndex)
        {
            base = ((IndexExpr*)base)->base_node;
        }

        if (base)
        {
            const TypeName* baseType = InferType(r, base, scope);

            if (baseType && TypeIsCString(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, a->base.range, "cannot assign into a 'cstring' (a constant string)");
                return false;
            }
        }
    }

    /* `.length` / `.cap` on arrays and strings are read-only views of
       the fat (fixed `.length` is a compile-time constant). */
    if (a->target->kind == NodeMember)
    {
        MemberExpr* fatM = (MemberExpr*)a->target;

        if (IsFatPseudoProperty(r, InferType(r, fatM->base_node, scope), fatM->member))
        {
            DiagErrorFmt(r->m_diag, a->base.range,
                         "cannot assign to '.%s' (read-only; arrays grow via array_push/array_resize)", fatM->member);
            return false;
        }
    }

    return true;
}

static bool ResolveAssignProperty(Resolver* r, AssignExpr* a, StrMap* scope)
{
    if (a->target->kind == NodeMember)
    {
        MemberExpr* tm = (MemberExpr*)a->target;
        PropertyDecl* prop = PropertyOnHandle(r, InferType(r, tm->base_node, scope), tm->member);

        if (prop)
        {
            const TypeName* rawBase = InferType(r, tm->base_node, scope);

            if (rawBase && TypeNameIsOptional(rawBase))
            {
                const char* baseKey = MovableBoxSourceKey(r, tm->base_node);
            }

            ResolveExpr(r, tm->base_node, scope);
            ResolveExpr(r, a->value, scope);

            /* The setter call produces no value: `a.P = b.Q = v` has no
               meaningful result (and an inner write used as a value would
               flow a void into the outer setter). */
            if (a->value->kind == NodeAssign && ((AssignExpr*)a->value)->isImplPropertyWrite)
            {
                DiagErrorFmt(r->m_diag, a->base.range,
                             "cannot chain property assignment; assign each property separately");
                return true;
            }

            a->isImplPropertyWrite = true;

            if (a->op != AssignSet)
            {
                DiagErrorFmt(r->m_diag, a->base.range, "cannot compound-assign to property '%s'; assign it directly",
                             tm->member);
                return true;
            }

            if (!prop->setterSymbol)
            {
                DiagErrorFmt(r->m_diag, a->base.range, "property '%s' is read-only", tm->member);
                return true;
            }

            const TypeName* vt = InferType(r, a->value, scope);

            if (vt && !IsAssignableType(r, &prop->returnType, vt)
                && !IsStrLiteralForCString(r, &prop->returnType, a->value))
            {
                DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to property '%s' of type '%s'", vt->name,
                             tm->member, prop->returnType.name);
            }

            return true;
        }
    }

    return false;
}

static bool AssignTargetIsOwningField(Resolver* r, AssignExpr* a, StrMap* scope)
{
    /* An owning field target is reassigned, not read - re-life it instead of tripping use-after-move. */
    const char* fieldKey = NULL;
    bool targetIsOwningField = false;

    if (a->target->kind == NodeMember)
    {
        const TypeName* ft = InferType(r, a->target, scope);

        if (ft && AliasIsOwning(r, ft))
        {
            targetIsOwningField = true;
            fieldKey = MovableBoxSourceKey(r, a->target);
        }

        // Writing through an optional needs proven ancestors.
        for (Node* anc = ((MemberExpr*)a->target)->base_node; anc;)
        {
            const TypeName* at2 = InferType(r, anc, scope);

            if (anc->kind == NodeMember)
            {
                anc = ((MemberExpr*)anc)->base_node;
            }
            else if (anc->kind == NodeIndex)
            {
                anc = ((IndexExpr*)anc)->base_node;
            }
            else
            {
                anc = NULL;
            }
        }
    }

    return targetIsOwningField;
}

static void ResolveAssignBracedRhs(Resolver* r, AssignExpr* a, StrMap* scope, const TypeName* tt)
{
    // A bare braced RHS takes its type from the target.
    if ((a->value->kind == NodeArrayInit || a->value->kind == NodeStructInit)
        && (a->target->kind == NodeIdent || a->target->kind == NodeMember || a->target->kind == NodeIndex))
    {
        const TypeName* at = (a->target->kind == NodeIdent) ? tt : InferType(r, a->target, scope);
        const TypeName* atArr = at && TypeNameIsOptional(at) ? at->inner : at;

        // A braced RHS against a struct target is a struct init.
        a->value = ApplyBracedStructTarget(r, a->value, atArr);

        if (a->value->kind == NodeArrayInit && at && TypeNameIsDynamicArray(atArr))
        {
            ArrayInitExpr* ai = (ArrayInitExpr*)a->value;

            if (!ai->elementType)
            {
                ai->elementType = TypeNameArrayElem(atArr);
            }
        }
        else if (a->value->kind == NodeStructInit && !((StructInitExpr*)a->value)->typeName)
        {
            const char* targetKey = MovableBoxSourceKey(r, a->target);

            DiagErrorFmt(r->m_diag, a->value->range,
                         "cannot infer a struct type for the braced initializer assigned to '%s' ('%s')",
                         targetKey ? targetKey : "target", at ? at->name : "unknown");
        }
        else if (a->value->kind == NodeArrayInit && at && !TypeNameIsFixedArray(atArr))
        {
            const char* targetKey = MovableBoxSourceKey(r, a->target);

            DiagErrorFmt(r->m_diag, a->value->range,
                         "cannot assign a braced initializer to '%s' of non-array type '%s'",
                         targetKey ? targetKey : "target", at->name);
        }
    }
}

static void ResolveAssignBoxTarget(Resolver* r, AssignExpr* a, StrMap* scope, const TypeName* tt)
{
    ResolveExpr(r, a->value, scope);

    if (a->op == AssignSet)
    {
        a->value = a->value;
    }

    const TypeName* vt = InferType(r, a->value, scope);

    // `=` rebinds the box; other ops mutate contents. Optionals always rebind.
    bool optionalTarget = TypeNameIsOptional(tt);

    if (optionalTarget && a->op != AssignSet)
    {
        DiagErrorFmt(r->m_diag, a->base.range,
                     "cannot compound-assign into '%s' of nullable type '%s'; it has not been blessed",
                     MovableBoxSourceKey(r, a->target), tt->name);

        return;
    }

    bool boxMove = a->op == AssignSet && vt && (strcmp(vt->name, tt->name) == 0 || optionalTarget);
    const char* targetName = MovableBoxSourceKey(r, a->target);

    if (boxMove)
    {
        if (IsBoxGlobalName(r, targetName))
        {
            DiagErrorFmt(r->m_diag, a->base.range,
                         "box global '%s' cannot be reassigned; only its fields may be mutated", targetName);

            return;
        }

        /* A `ref ^T` is the caller's box: assign contents, not the slot. A `ref` to a `T?`
           (or a string) is the caller's slot: `=` drops what it held and
           stores the new value there, so out-params work. */
        bool slotRef = TypeNameIsOptional(tt) || TypeIsString(&r->m_registry, tt->name) || TypeNameIsDynamicArray(tt);

        if (StrMapGet(&r->m_refBoxParams, targetName) && !slotRef)
        {
            DiagErrorFmt(r->m_diag, a->base.range,
                         "'%s' cannot be reassigned as it is not owned because it is bound as a ref; "
                         "assign its inner value instead",
                         targetName);

            return;
        }

        const char* movedValueKey = MovableBoxSourceKey(r, a->value);

        if (movedValueKey)
        {
            if (vt && TypeNameIsOptional(vt))
            {
                // Moving from `T?` empties the source.
                MoveOptionalSource(r, movedValueKey, a->base.range);
            }
            else
            {
                MarkMoved(r, movedValueKey, a->base.range);
            }
        }
    }
    else
    {
        // Contents-assign into ^T (not a move).
        const TypeName* inner = TypeNameBoxInner(tt);

        if (vt && inner && !IsAssignableType(r, inner, vt))
        {
            DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' into '%s' '%s'", vt->name, inner->name,
                         targetName);
        }

        // Contents-assign reads through the optional.

        /* An owning-struct inner (`^Rec b; b = arr[i];`) REPLACES the
           cell contents, so the RHS value moves in: the source is
           consumed (codegen drops the old contents, then constructs
           the new value into the cell). */
        if (a->op == AssignSet && inner && vt && TypeRegistryIsOwningStruct(&r->m_registry, inner->name)
            && AliasIsOwningValue(r, vt))
        {
            const char* movedValueKey = MovableBoxSourceKey(r, a->value);

            if (movedValueKey)
            {
                MarkMoved(r, movedValueKey, a->base.range);
            }
        }

        // New contents: facts on the box's sub-paths (`h.f`) are stale.
    }
}

static void ResolveAssignOwningFieldTarget(Resolver* r, AssignExpr* a, StrMap* scope)
{
    // Writing an owning field re-lives it.
    Node* root = a->target;

    while (root->kind == NodeMember || root->kind == NodeIndex)
    {
        root = (root->kind == NodeMember) ? ((MemberExpr*)root)->base_node : ((IndexExpr*)root)->base_node;
    }

    if (root->kind == NodeIdent)
    {
        ResolveExprImpl(r, root, scope, true);
    }

    if (a->op == AssignSet)
    {
        // Plain `=` restores the field and healed ancestors.
    }
    else
    {
        // Compound ops read first, so the field must be live.
        ResolveExpr(r, a->target, scope);
    }

    ResolveExpr(r, a->value, scope);

    if (a->op == AssignSet)
    {
        a->value = a->value;
        const TypeName* vt = InferType(r, a->value, scope);
        const char* movedValueKey = MovableBoxSourceKey(r, a->value);

        // An owning RHS moves out of its source, like box-to-box.
        if (movedValueKey)
        {
            if (vt && TypeNameIsOptional(vt))
            {
                MoveOptionalSource(r, movedValueKey, a->base.range);
            }
            else
            {
                MarkMoved(r, movedValueKey, a->base.range);
            }
        }
    }
}

static void ResolveAssignPlainTarget(Resolver* r, AssignExpr* a, StrMap* scope, const TypeName* tt)
{
    ResolveExpr(r, a->target, scope);
    ResolveExpr(r, a->value, scope);

    if (a->op == AssignSet)
    {
        a->value = a->value;
    }

    const char* writtenKey = MovableBoxSourceKey(r, a->target);

    // Enum constants are read-only.
    if (a->target->kind == NodeMember && ((MemberExpr*)a->target)->isEnumConst)
    {
        DiagErrorFmt(r->m_diag, a->base.range, "cannot assign to enum constant '%s.%s'",
                     ((MemberExpr*)a->target)->enumTypeName, ((MemberExpr*)a->target)->member);
        return;
    }

    // Scalar pseudo-properties (`int.max`) are read-only.
    if (a->target->kind == NodeMember && ((MemberExpr*)a->target)->isScalarConst)
    {
        MemberExpr* sm = (MemberExpr*)a->target;
        DiagErrorFmt(r->m_diag, a->base.range, "cannot assign to '%s.%s' (a constant)",
                     ((IdentExpr*)sm->base_node)->name, sm->member);
        return;
    }

    // Whole fixed arrays assign by element, never as a unit (locals,
    // fields and elements alike — braced initialization at the
    // declaration is the only whole-array form).
    if (a->target->kind == NodeMember || a->target->kind == NodeIndex || a->target->kind == NodeIdent)
    {
        const TypeName* mt = InferType(r, a->target, scope);

        if (TypeNameIsFixedArray(mt))
        {
            DiagErrorFmt(r->m_diag, a->base.range,
                         "cannot assign to a whole fixed-size array of type '%s'; assign its elements instead",
                         mt->name);
        }
    }

    // `^T` into `T` reads through the box (not a move).
    if (tt)
    {
        const TypeName* vt = InferType(r, a->value, scope);

        if (vt && !IsAssignableType(r, tt, vt) && !IsStrLiteralForCString(r, tt, a->value))
        {
            DiagErrorFmt(r->m_diag, a->base.range, "cannot assign '%s' to '%s' of type '%s'", vt->name,
                         ((IdentExpr*)a->target)->name, tt->name);
        }

        /* Assigning a `T?` into a plain `T` unwraps the optional. */
    }
    else if (a->target->kind == NodeIndex && a->op == AssignSet)
    {
        /* Element write into an owning-element array (`Rec[]`,
           `string[]`): the old element is dropped and the RHS moves
           in. A borrow out of another array element would leave the
           value owned by two arrays — same rule as push. */
        const TypeName* et = InferType(r, a->target, scope);
        const TypeName* vt = InferType(r, a->value, scope);

        if (et && AliasIsOwningValue(r, et) && !TypeNameIsBox(et))
        {
            if (vt && AliasIsOwningValue(r, vt) && IsArrayElementBorrow(a->value))
            {
                DiagErrorFmt(r->m_diag, a->value->range,
                             "cannot assign '%s' read from an array element - it would be owned by two "
                             "arrays; move it into a variable first",
                             vt->name);
            }

            const char* movedValueKey = vt && AliasIsOwningValue(r, vt) ? MovableBoxSourceKey(r, a->value) : NULL;

            if (movedValueKey)
            {
                if (TypeNameIsOptional(vt))
                {
                    MoveOptionalSource(r, movedValueKey, a->base.range);
                }
                else
                {
                    MarkMoved(r, movedValueKey, a->base.range);
                }
            }
        }
        else if (et && TypeNameIsBox(et) && vt && TypeNameIsBox(vt))
        {
            /* `^T` element from a `^T` value: codegen moves the box
               pointer in and nulls the source (a variable, field or
               another element), so record the move. */
            const char* movedValueKey = MovableBoxSourceKey(r, a->value);

            if (movedValueKey)
            {
                MarkMoved(r, movedValueKey, a->base.range);
            }
        }
    }
    else if (a->target->kind == NodeMember)
    {
        /* Non-owning field target (`bob.plain = opt;`) - the field
            type is the unwrap target. */
        const TypeName* mt = InferType(r, a->target, scope);
        const TypeName* vt = InferType(r, a->value, scope);
    }

    /* The written path holds a new value: every fact at, under, or
       aliasing it is stale. `=` re-proves a `T?` target when the
       value is provably non-empty. */
    if (writtenKey)
    {

        const TypeName* writtenType = InferType(r, a->target, scope);
    }
}

static void ResolveAssignExpr(Resolver* r, Node* n, StrMap* scope)
{
    AssignExpr* a = (AssignExpr*)n;

    if (!a->target || !a->value)
    {
        return;
    }

    CheckConstAssign(r, a->target, a->base.range, scope);

    if (!CheckAssignTargetForm(r, a, scope))
    {
        return;
    }

    if (ResolveAssignProperty(r, a, scope))
    {
        return;
    }

    /* Reassigning a scalar local invalidates its index spellings' facts. */
    if (a->target->kind == NodeIdent)
    {
    }
    else if (a->target->kind == NodeIndex)
    {
        /* Writing an array element kills nested-index/length facts of the array root. */
        const char* targetKey = MovableBoxSourceKey(r, a->target);

        if (targetKey)
        {
        }
    }

    const TypeName* tt = (a->target->kind == NodeIdent) ? InferType(r, a->target, scope) : NULL;
    bool targetIsBox = tt && AliasIsOwning(r, tt);

    /* A `ref T[]` param may be a borrowed stack view (fixed-array slice):
       rebinding drops/frees the old buffer — the caller's storage. */
    if (tt && a->target->kind == NodeIdent && TypeNameIsDynamicArray(tt)
        && StrMapGet(&r->m_refArrayParams, ((IdentExpr*)a->target)->name)
        && !IsGrowableArrayParam(r, ((IdentExpr*)a->target)->name))
    {
        DiagErrorFmt(r->m_diag, a->base.range,
                     "'%s' cannot be reassigned as it is not owned because it is bound as a ref array param; "
                     "mutate its elements instead",
                     ((IdentExpr*)a->target)->name);

        return;
    }
    bool targetIsOwningField = AssignTargetIsOwningField(r, a, scope);

    ResolveAssignBracedRhs(r, a, scope, tt);

    if (targetIsBox)
    {
        ResolveAssignBoxTarget(r, a, scope, tt);
    }
    else if (targetIsOwningField)
    {
        ResolveAssignOwningFieldTarget(r, a, scope);
    }
    else
    {
        ResolveAssignPlainTarget(r, a, scope, tt);
    }
}

static void ResolveIncDecExpr(Resolver* r, Node* n, StrMap* scope)
{
    IncDecExpr* inc = (IncDecExpr*)n;

    /* Properties have no addressable storage: ++/-- would need a
       get+set pair; require explicit assignment instead. */
    if (inc->operand->kind == NodeMember)
    {
        MemberExpr* tm = (MemberExpr*)inc->operand;

        if (PropertyOnHandle(r, InferType(r, tm->base_node, scope), tm->member))
        {
            DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s property '%s'; assign it explicitly instead",
                         inc->isDec ? "decrement" : "increment", tm->member);
            return;
        }

        if (IsFatPseudoProperty(r, InferType(r, tm->base_node, scope), tm->member))
        {
            DiagErrorFmt(r->m_diag, inc->base.range,
                         "cannot %s '.%s' (read-only; arrays grow via array_push/array_resize)",
                         inc->isDec ? "decrement" : "increment", tm->member);
            return;
        }
    }

    CheckConstAssign(r, inc->operand, inc->base.range, scope);
    ResolveExpr(r, inc->operand, scope);

    /* ++/-- on an element of constant `cstring` storage is rejected. */
    if (inc->operand->kind == NodeIndex)
    {
        Node* base = inc->operand;

        while (base && base->kind == NodeIndex)
        {
            base = ((IndexExpr*)base)->base_node;
        }

        if (base)
        {
            const TypeName* baseType = InferType(r, base, scope);

            if (baseType && TypeIsCString(&r->m_registry, baseType->name))
            {
                DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s an element of a 'cstring' (a constant string)",
                             inc->isDec ? "decrement" : "increment");
                return;
            }
        }
    }

    /* ++/-- only makes sense on numeric storage; anything else would
       fall through to pointer arithmetic. Bool is not numeric: `b++`
       wraps the i1 (true -> false). */
    const TypeName* incType = InferType(r, inc->operand, scope);

    if (incType && (!IsNumeric(incType->primitiveType) || IsBoolType(&r->m_registry, incType)))
    {
        DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s a value of type '%s' (expected a numeric type)",
                     inc->isDec ? "decrement" : "increment", incType->name);
    }

    // Scalar pseudo-properties (`int.max`) are read-only.
    if (inc->operand->kind == NodeMember && ((MemberExpr*)inc->operand)->isScalarConst)
    {
        MemberExpr* sm = (MemberExpr*)inc->operand;
        DiagErrorFmt(r->m_diag, inc->base.range, "cannot %s '%s.%s' (a constant)",
                     inc->isDec ? "decrement" : "increment", ((IdentExpr*)sm->base_node)->name, sm->member);
        return;
    }

    /* ++/-- invalidates index spellings of the operand. */
    if (inc->operand->kind == NodeIdent)
    {
    }
    else if (inc->operand->kind == NodeIndex)
    {
        /* ++/-- on an array element feeds nested-index spellings too. */
        const char* operandKey = MovableBoxSourceKey(r, inc->operand);

        if (operandKey)
        {
        }
    }

    return;
}

static void ResolveCastExpr(Resolver* r, Node* n, StrMap* scope)
{
    CastExpr* cast = (CastExpr*)n;

    ResolveExpr(r, cast->operand, scope);

    const TypeName* src = InferType(r, cast->operand, scope);
    const TypeName* dst = &cast->type;

    CheckConstFloatToInt(r->m_diag, cast);

    const char* srcName = src ? src->name : "";
    const char* dstName = dst->name;

    /* Resolve type aliases to their underlying types for cast compatibility. */
    const char* resolvedSrc = TypeRegistryResolveAlias(&r->m_registry, srcName);
    const char* resolvedDst = TypeRegistryResolveAlias(&r->m_registry, dstName);

    bool scalarPair = src && (IsScalarLikeType(&r->m_registry, srcName) && IsScalarLikeType(&r->m_registry, dstName));
    bool handlePair = src && IsHandleType(&r->m_registry, srcName) && IsHandleType(&r->m_registry, dstName)
                      && (HandleExtendsFrom(&r->m_registry, dstName, srcName)
                          || HandleExtendsFrom(&r->m_registry, srcName, dstName));

    /* SIMD vector pair: same lane count (including through type aliases). */
    const int srcLanes = IsNameSimdVector(resolvedSrc);
    const int dstLanes = IsNameSimdVector(resolvedDst);
    bool simdPair = src && srcLanes != 0 && dstLanes != 0 && srcLanes == dstLanes;

    /* ^T -> ^U only when T or U is opaque (erase/cast-back). */
    bool boxPair = src && TypeNameIsOwning(src) && TypeNameIsOwning(dst)
                   && ((TypeNameBoxInner(src) && TypeRegistryIsOpaque(&r->m_registry, src->inner->name))
                       || (TypeNameBoxInner(dst) && TypeRegistryIsOpaque(&r->m_registry, dst->inner->name)));

    /* Alias <-> underlying (or alias <-> alias) of the SAME resolved type:
       the explicit escape hatch for strong typedefs (`(Name)s`, `(string)n`).
       Distinct aliases still never convert implicitly. */
    bool aliasPair
        = src && SameResolvedType(r, srcName, dstName)
          && (TypeRegistryIsTypeAlias(&r->m_registry, srcName) || TypeRegistryIsTypeAlias(&r->m_registry, dstName));

    /* `cstring` -> `string` copies (heap copy at the use site);
       a string literal casts to `cstring` (contextual `.rodata` pointer). */
    bool cstringStringPair = src
                             && ((TypeIsCString(&r->m_registry, srcName) && TypeIsString(&r->m_registry, dstName))
                                 || (cast->operand->kind == NodeStrLiteral && TypeIsCString(&r->m_registry, dstName)));

    /* Handle <-> integer: a handle reads back as its 64-bit address (`(ulong)h`, `(long)h`), and
       any integer converts to a handle (`(ptr)addr`). */
    PrimitiveType srcPrim = GetPrimitiveType(resolvedSrc);
    PrimitiveType dstPrim = GetPrimitiveType(resolvedDst);
    bool handleIntPair = src
                         && ((IsHandleType(&r->m_registry, srcName) && (dstPrim == PrimLong || dstPrim == PrimULong))
                             || (IsHandleType(&r->m_registry, dstName) && IsNumeric(srcPrim)
                                 && !IsNameFloatType(resolvedSrc) && srcPrim != PrimBool));

    /* `(cstring)p`: view raw memory as a NUL-terminated string; `(ptr)cs` the reverse. */
    bool cstringPtrPair = src
                          && ((TypeIsCString(&r->m_registry, dstName) && strcmp(resolvedSrc, STRATA_PTR_TYPE) == 0)
                              || (TypeIsCString(&r->m_registry, srcName) && strcmp(resolvedDst, STRATA_PTR_TYPE) == 0));

    /* Text views: a NUL-terminated `byte[N]` buffer as a `cstring` (borrowed) or a `string`
       (copied, up to the first NUL or all N bytes); a `string` as a `cstring` (borrowed). */
    bool bufferSrc = src && TypeNameHasArrayShape(src) && src->length >= 0 && src->elem
                     && !TypeNameHasArrayShape(src->elem)
                     && (strcmp(src->elem->name, "byte") == 0 || strcmp(src->elem->name, "sbyte") == 0);
    cast->textKind = CastTextNone;

    if (bufferSrc && TypeIsCString(&r->m_registry, dstName))
    {
        cast->textKind = CastBufferToCString;
    }
    else if (bufferSrc && TypeIsString(&r->m_registry, dstName))
    {
        cast->textKind = CastBufferToString;
    }
    else if (src && cast->operand->kind != NodeStrLiteral && TypeIsString(&r->m_registry, srcName)
             && TypeIsCString(&r->m_registry, dstName))
    {
        /* The cstring would point into the string's buffer and dangle once the string is dropped or grows. */
        DiagErrorFmt(r->m_diag, cast->base.range,
                     "a string cannot be cast to cstring: the result would dangle when the string is dropped; "
                     "pass the string itself where C expects text");
        cast->textKind = CastStringToCString;
    }
    else if (src && TypeNameIsFixedArray(src) && strcmp(resolvedDst, STRATA_PTR_TYPE) == 0)
    {
        cast->textKind = CastFixedToPtr;
    }

    if (src && !scalarPair && !handlePair && !simdPair && !boxPair && !aliasPair && !cstringStringPair && !handleIntPair
        && !cstringPtrPair && cast->textKind == CastTextNone)
    {
        DiagErrorFmt(r->m_diag, cast->base.range, "invalid cast from '%s' to '%s'", srcName, dstName);
    }

    /* Casting a compile-time constant to an enum: the value must fit the
       enum's underlying range (`(Color)300` for a byte-based enum). */
    if (TypeRegistryIsEnum(&r->m_registry, dstName))
    {
        uint64_t mag = 0;
        bool neg = false;

        if (ResolvedConstIntValue(cast->operand, &mag, &neg) && !EnumConstFits(resolvedDst, mag, neg))
        {
            DiagErrorFmt(r->m_diag, cast->base.range, "enum value does not fit in underlying type '%s'", resolvedDst);
        }
    }

    return;
}

static void ResolveConditionalExpr(Resolver* r, Node* n, StrMap* scope)
{
    ConditionalExpr* ce = (ConditionalExpr*)n;

    ResolveExpr(r, ce->condition, scope);
    CheckBoolCondition(r, ce->condition, scope);
    ResolveExpr(r, ce->thenExpr, scope);
    ResolveExpr(r, ce->elseExpr, scope);
    ce->resultType = ConditionalType(r, ce, scope, true);
    return;
}

static void ResolveNullTestExpr(Resolver* r, Node* n, StrMap* scope)
{
    NullTestExpr* nt = (NullTestExpr*)n;

    // Optional ancestors of a `?` test must already be proven.
    Node* anc = NULL;

    if (nt->operand->kind == NodeMember)
    {
        anc = ((MemberExpr*)nt->operand)->base_node;
    }
    else if (nt->operand->kind == NodeIndex)
    {
        anc = ((IndexExpr*)nt->operand)->base_node;
    }

    for (; anc;)
    {
        const TypeName* at = InferType(r, anc, scope);

        if (anc->kind == NodeMember)
        {
            anc = ((MemberExpr*)anc)->base_node;
        }
        else if (anc->kind == NodeIndex)
        {
            anc = ((IndexExpr*)anc)->base_node;
        }
        else
        {
            anc = NULL;
        }
    }

    // Resolve the operand path itself.
    ResolveExpr(r, nt->operand, scope);

    const TypeName* operandType = InferType(r, nt->operand, scope);

    if (operandType && !TypeNameIsOptional(operandType))
    {
        DiagErrorFmt(r->m_diag, nt->base.range, "'?' test requires a nullable type ('T?'), but '%s' can never be empty",
                     operandType->name);
    }

    return;
}

void ResolveExprImpl(Resolver* r, Node* n, StrMap* scope, bool asMemberBase)
{
    if (!n)
    {
        return;
    }

    r->m_curScope = scope;

    switch (n->kind)
    {
    case NodeIntLiteral:
    case NodeFloatLiteral:
    case NodeBoolLiteral:
    case NodeStrLiteral:
        return;
    case NodeIdent:
        ResolveIdentExpr(r, n, scope);
        return;
    case NodeUnary:
        ResolveUnaryExpr(r, n, scope);
        return;
    case NodeBinary:
        ResolveBinaryExpr(r, n, scope);
        return;
    case NodeAssign:
        ResolveAssignExpr(r, n, scope);
        return;
    case NodeIncDec:
        ResolveIncDecExpr(r, n, scope);
        return;
    case NodeCast:
        ResolveCastExpr(r, n, scope);
        return;
    case NodeMember:
        ResolveMemberExpr(r, n, scope);
        return;
    case NodeConditional:
        ResolveConditionalExpr(r, n, scope);
        return;
    case NodeCall:
        ResolveCallExpr(r, n, scope);
        return;
    case NodeStructInit:
        ResolveStructInitExpr(r, n, scope);
        return;
    case NodeIndex:
        ResolveIndexExpr(r, n, scope);
        return;
    case NodeNullTest:
        ResolveNullTestExpr(r, n, scope);
        return;
    case NodeArrayInit:
        ResolveArrayInitExpr(r, n, scope);
        return;
    default:
        return;
    }
}
