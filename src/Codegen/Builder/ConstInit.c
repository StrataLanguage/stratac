/* Builder: Constant folding for global initializers. */
#include "Codegen/Builder/BuilderInternal.h"

/* The runtime integer type of a scalar TypeDesc. Non-integers (and i1 bools) carry no narrowing. */
static ConstIntType ConstIntTypeOfTd(TypeDesc td)
{
    if (td.type && !TypeDescIsFloat(&td) && LLVMGetTypeKind(td.type) == LLVMIntegerTypeKind
        && LLVMGetIntTypeWidth(td.type) > 1)
    {
        return (ConstIntType){LLVMGetIntTypeWidth(td.type), TypeDescIsUnsigned(&td)};
    }

    return (ConstIntType){0, false};
}

/* Convert a folded constant to `td` (const-level mirror of Coerce). An integer target narrows the value to its
 * width and signedness, as the emitted conversion does. */
ConstInitVal CastConstVal(Builder* b, ConstInitVal v, TypeDesc td)
{
    if (td.type == I1Ty(b)) /* conversion to bool is a (non)zero test */
    {
        ConstInitVal r;
        r.kind = CIK_BOOL;
        r.i = (v.kind == CIK_FLOAT ? v.f != 0.0 : v.i != 0) ? 1 : 0;
        r.f = 0.0;
        r.ity = (ConstIntType){0, false};
        return r;
    }

    if (TypeDescIsFloat(&td))
    {
        if (v.kind == CIK_FLOAT)
        {
            return v;
        }

        ConstInitVal r;
        r.kind = CIK_FLOAT;
        r.i = 0;
        r.f = (double)(ConstInt)v.i;
        r.ity = (ConstIntType){0, false};
        return r;
    }

    ConstInitVal r = v;

    r.kind = CIK_INT;
    if (v.kind == CIK_FLOAT)
    {
        r.i = (ConstUInt)(ConstInt)v.f;
    }
    r.f = 0.0;
    r.ity = ConstIntTypeOfTd(td);
    r.i = ConstIntNarrow(r.i, r.ity);
    return r;
}

/* Folds an integer operator at the type EmitBinary computes in: both operands are converted to it first. */
static bool FoldIntBinary(BinaryOp op, ConstInitVal l, ConstInitVal r, ConstInitVal* out)
{
    ConstIntType t = ConstIntTypeCommon(l.ity, r.ity);
    uint64_t v;

    if (!ConstFoldIntAt(op, ConstIntNarrow(l.i, t), ConstIntNarrow(r.i, t), t, &v))
    {
        return false;
    }

    out->kind = CIK_INT;
    out->i = v;
    out->f = 0.0;
    out->ity = t;
    return true;
}

bool FoldIntLiteralInit(Node* n, ConstInitVal* out)
{
    /* Typed as the checker types it (InferType): `l` suffixes and values past i32 are 64-bit. */
    IntLiteral* lit = AsNode(IntLiteral, n);

    *out = (ConstInitVal){CIK_INT, .i = lit->value, .f = 0.0};
    out->ity = (ConstIntType){lit->isLong || lit->value > 0x7FFFFFFFULL ? 64u : 32u, lit->isUnsigned};
    return true;
}

bool FoldFloatLiteralInit(Node* n, ConstInitVal* out)
{
    *out = (ConstInitVal){CIK_FLOAT, .i = 0, .f = ((FloatLiteral*)n)->value};
    return true;
}

bool FoldBoolLiteralInit(Node* n, ConstInitVal* out)
{
    *out = (ConstInitVal){CIK_BOOL, .i = ((BoolLiteral*)n)->value ? 1 : 0, .f = 0.0};
    return true;
}

/* Unary operators over a folded operand. */
bool FoldUnaryInit(Builder* b, TypeDesc td, Node* n, ConstInitVal* out)
{
    UnaryExpr* u = AsNode(UnaryExpr, n);
    ConstInitVal inner;

    if (!FoldConstInit(b, td, u->operand, &inner))
    {
        return false;
    }

    switch (u->op)
    {
    case UnNeg:
        if (inner.kind == CIK_FLOAT)
        {
            inner.f = -inner.f;
        }
        else
        {
            inner.i = ConstIntNarrow(0ULL - inner.i, inner.ity);
        }

        break;
    case UnPos:
        break;
    case UnNot:
    {
        bool isZero = (inner.kind == CIK_FLOAT) ? inner.f == 0.0 : inner.i == 0;

        inner.kind = CIK_INT;
        inner.i = isZero;
        inner.f = 0.0;
        inner.ity = (ConstIntType){0, false};
        break;
    }
    case UnBitNot:
        if (inner.kind == CIK_FLOAT)
        {
            return false;
        }

        inner.kind = CIK_INT;
        inner.i = ConstIntNarrow(~inner.i, inner.ity);
        break;
    default:
        return false;
    }

    *out = inner;
    return true;
}

/* Binary operators over two folded operands, computed in the type EmitBinary uses. */
bool FoldBinaryInit(Builder* b, TypeDesc td, Node* n, ConstInitVal* out)
{
    BinaryExpr* e = AsNode(BinaryExpr, n);
    ConstInitVal l;
    ConstInitVal r;

    if (!FoldConstInit(b, td, e->lhs, &l) || !FoldConstInit(b, td, e->rhs, &r))
    {
        return false;
    }

    bool lFloat = l.kind == CIK_FLOAT;
    bool rFloat = r.kind == CIK_FLOAT;

    switch (e->op)
    {
    case BinAdd:
    case BinSub:
    case BinMul:
    case BinDiv:
    {
        if (lFloat || rFloat)
        {
            double a = lFloat ? l.f : (double)(ConstInt)l.i;
            double z = rFloat ? r.f : (double)(ConstInt)r.i;

            out->kind = CIK_FLOAT;
            out->i = 0;
            out->ity = (ConstIntType){0, false};
            return ConstFoldFloatArith(e->op, a, z, &out->f);
        }

        return FoldIntBinary(e->op, l, r, out);
    }
    case BinMod:
    case BinBitAnd:
    case BinBitOr:
    case BinBitXor:
    case BinShl:
    case BinShr:
    {
        if (lFloat || rFloat)
        {
            return false;
        }

        return FoldIntBinary(e->op, l, r, out);
    }
    case BinEqEq:
    case BinNotEq:
    case BinLt:
    case BinLtEq:
    case BinGt:
    case BinGtEq:
    {
        bool res;

        if (lFloat || rFloat)
        {
            double a = lFloat ? l.f : (double)(ConstInt)l.i;
            double z = rFloat ? r.f : (double)(ConstInt)r.i;

            if (!ConstFoldFloatCompare(e->op, a, z, &res))
            {
                return false;
            }
        }
        else if (!ConstFoldIntCompare(e->op, (ConstInt)l.i, (ConstInt)r.i, &res))
        {
            return false;
        }

        out->kind = CIK_INT;
        out->i = res ? 1 : 0;
        out->f = 0.0;
        out->ity = (ConstIntType){0, false};
        return true;
    }
    case BinLogicAnd:
    case BinLogicOr:
    {
        bool a = lFloat ? l.f != 0.0 : l.i != 0;
        bool z = rFloat ? r.f != 0.0 : r.i != 0;

        out->kind = CIK_INT;
        out->i = (e->op == BinLogicAnd) ? (a && z) : (a || z);
        out->f = 0.0;
        out->ity = (ConstIntType){0, false};
        return true;
    }
    default:
        return false;
    }
}

bool FoldCastInit(Builder* b, Node* n, ConstInitVal* out)
{
    CastExpr* c = AsNode(CastExpr, n);
    TypeDesc innerTd = Resolve(b, &c->type);

    if (TypeDescIsVoid(&innerTd) || innerTd.structTypeName || TypeDescHasArrayShape(&innerTd) || TypeDescIsBox(&innerTd)
        || TypeDescIsSimdVector(&innerTd))
    {
        return false;
    }

    ConstInitVal inner;

    if (!FoldConstInit(b, innerTd, c->operand, &inner))
    {
        return false;
    }

    /* The cast's own target type is the only conversion here (the truncation the user asked for, e.g.
       `(byte)-8`). The enclosing global's type is applied by the caller: converting here would retype a
       nested cast, as in `(uint)-1 >> 1` inside an `int` constant. */
    *out = CastConstVal(b, inner, innerTd);
    return true;
}

bool FoldIdentInit(Builder* b, Node* n, ConstInitVal* out)
{
    /* Chained manifest constants: `const B = A;` */
    ConstValueSlot* cv = (ConstValueSlot*)StrMapGet(&b->constValues, ((IdentExpr*)n)->name);

    if (!cv)
    {
        return false;
    }

    *out = cv->civ;
    return true;
}

bool FoldMemberInit(Builder* b, Node* n, ConstInitVal* out)
{
    /* Scoped enum constant: `EnumName.Member` folds to its underlying
       integer value (marked by sema). */
    MemberExpr* m = (MemberExpr*)n;

    if (m->isEnumConst)
    {
        /* Typed by the enum's underlying integer type, as EmitMember emits it. */
        TypeName enumTn = TypeNameLeaf((char*)m->enumTypeName);

        *out = (ConstInitVal){CIK_INT, .i = m->enumValue, .f = 0.0};
        out->ity = ConstIntTypeOfTd(Resolve(b, &enumTn));
        return true;
    }

    /* Builtin scalar pseudo-property: `int.max` / `float.min` (marked by
       sema) folds to the C-limit constant. */
    if (m->isScalarConst)
    {
        uint64_t intVal = 0;
        double floatVal = 0.0;
        bool isFloat = false;

        if (m->baseNode->kind == NodeIdent
            && ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)m->baseNode)->name), m->member, &intVal, &floatVal,
                                 &isFloat))
        {
            *out = isFloat ? (ConstInitVal){CIK_FLOAT, .i = 0, .f = floatVal}
                           : (ConstInitVal){CIK_INT, .i = intVal, .f = 0.0};
            /* Typed as the base scalar itself, as EmitMember emits it. */
            TypeName baseTn = TypeNameLeaf((char*)((IdentExpr*)m->baseNode)->name);
            out->ity = ConstIntTypeOfTd(Resolve(b, &baseTn));
            return true;
        }
    }

    return false;
}

// Fold a global initializer; false when not a compile-time constant.
bool FoldConstInit(Builder* b, TypeDesc td, Node* n, ConstInitVal* out)
{
    switch (n->kind)
    {
    case NodeIntLiteral:
        return FoldIntLiteralInit(n, out);
    case NodeFloatLiteral:
        return FoldFloatLiteralInit(n, out);
    case NodeBoolLiteral:
        return FoldBoolLiteralInit(n, out);
    case NodeUnary:
        return FoldUnaryInit(b, td, n, out);
    case NodeBinary:
        return FoldBinaryInit(b, td, n, out);
    case NodeCast:
        return FoldCastInit(b, n, out);
    case NodeIdent:
        return FoldIdentInit(b, n, out);
    case NodeMember:
        return FoldMemberInit(b, n, out);
    default:
        return false;
    }
}