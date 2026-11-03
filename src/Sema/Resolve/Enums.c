// Enum member lookup and enum member value evaluation.
#include "Sema/Resolve/ResolverInternal.h"

#include <inttypes.h>

// The enum an expression belongs to: its type, or the enum of a `Color.Red` constant. NULL for a non-enum.
const char* ExprEnumName(Resolver* r, const Node* n, const TypeName* t)
{
    if (n->kind == NodeMember && ((const MemberExpr*)n)->isEnumConst)
    {
        return ((const MemberExpr*)n)->enumTypeName;
    }

    if (t && t->name && TypeRegistryIsEnum(&r->registry, t->name))
    {
        return t->name;
    }

    return NULL;
}

// `a & b`, `a | b`, `a ^ b` on two values of one enum type: flag sets keep their enum type.
bool IsEnumFlagsOp(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt)
{
    if (op != BinBitAnd && op != BinBitOr && op != BinBitXor)
    {
        return false;
    }

    if (!lt || !rt || strcmp(lt->name, rt->name) != 0)
    {
        return false;
    }

    const StructType* st = TypeRegistryFind(&r->registry, lt->name);

    return st && st->isEnum && st->isFlagsEnum;
}

// Is `name` an enum that is not declared `@flags`?
bool IsPlainEnum(const Resolver* r, const char* name)
{
    const StructType* st = name ? TypeRegistryFind(&r->registry, name) : NULL;

    return st && st->isEnum && !st->isFlagsEnum;
}

// Resolves `EnumName.Member` — a scoped enum constant read. Returns true when
// the member expression was handled (marked as an enum constant, or diagnosed
// as a bad member). Only bare type-name bases are considered; a variable
// shadowing the enum name takes precedence.
bool TryResolveEnumMember(Resolver* r, MemberExpr* m, StrMap* scope)
{
    if (m->baseNode->kind != NodeIdent)
    {
        return false;
    }

    const char* baseName = ((IdentExpr*)m->baseNode)->name;

    if (StrMapGet(scope, baseName))
    {
        return false; // a local shadows the enum type name
    }

    if (!TypeRegistryIsEnum(&r->registry, baseName))
    {
        return false;
    }

    const EnumDecl* ed = ModuleFindEnum(r->mod, baseName);

    if (!ed)
    {
        return false;
    }

    for (size_t i = 0; i < ed->members.count; i++)
    {
        EnumMemberDecl* member = (EnumMemberDecl*)VecGet(&ed->members, i);

        if (strcmp(member->name, m->member) == 0)
        {
            m->isEnumConst = true;
            m->enumValue = member->value;
            m->enumTypeName = baseName;
            return true;
        }
    }

    DiagErrorFmt(r->diag, m->base.range, "enum '%s' has no member '%s'", baseName, m->member);
    return true;
}

// Finds `memberName` in enum `ed`; returns true with `*outIndex` set.
static bool FindEnumMemberIndex(EnumDecl* ed, const char* memberName, size_t* outIndex)
{
    for (size_t i = 0; i < ed->members.count; i++)
    {
        EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, i);

        if (strcmp(m->name, memberName) == 0)
        {
            *outIndex = i;
            return true;
        }
    }

    return false;
}

static EnumEvalStatus EnumEvalConstExpr(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, bool isUnsigned,
                                        ConstUInt* out);

static EnumEvalStatus EnumEvalIntLiteral(Node* n, ConstUInt* out)
{
    *out = ((IntLiteral*)n)->value;
    return EnumEvalOk;
}

static EnumEvalStatus EnumEvalBoolLiteral(Node* n, ConstUInt* out)
{
    *out = ((BoolLiteral*)n)->value ? 1 : 0;
    return EnumEvalOk;
}

static EnumEvalStatus EnumEvalUnary(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, bool isUnsigned,
                                    ConstUInt* out)
{
    UnaryExpr* u = (UnaryExpr*)n;
    ConstUInt inner = 0;
    EnumEvalStatus st = EnumEvalConstExpr(u->operand, mod, enumIndex, memberIndex, isUnsigned, &inner);

    if (st != EnumEvalOk)
    {
        return st;
    }

    switch (u->op)
    {
    case UnPos:
        *out = inner;
        return EnumEvalOk;
    case UnNeg:
        if (isUnsigned)
        {
            // `-1u` on an unsigned underlying wraps C-style to 0xFFFFFFFF
            // (a plain `-1` stays rejected).
            if (ConstBitWidth(u->operand) == 32)
            {
                *out = (0 - inner) & 0xFFFFFFFFULL;
                return EnumEvalOk;
            }

            return EnumEvalNegForUnsigned;
        }

        *out = (ConstUInt)(0 - inner);
        return EnumEvalOk;
    case UnBitNot:
        *out = ConstBitWidth(u->operand) == 32 ? (~inner & 0xFFFFFFFFULL) : ~inner;
        return EnumEvalOk;
    default:
        return EnumEvalNotConst;
    }
}

static EnumEvalStatus EnumEvalBinary(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, bool isUnsigned,
                                     ConstUInt* out)
{
    BinaryExpr* e = (BinaryExpr*)n;
    ConstUInt l = 0;
    ConstUInt r = 0;
    EnumEvalStatus sl = EnumEvalConstExpr(e->lhs, mod, enumIndex, memberIndex, isUnsigned, &l);

    if (sl != EnumEvalOk)
    {
        return sl;
    }

    EnumEvalStatus sr = EnumEvalConstExpr(e->rhs, mod, enumIndex, memberIndex, isUnsigned, &r);

    if (sr != EnumEvalOk)
    {
        return sr;
    }

    switch (e->op)
    {
    case BinAdd:
        *out = l + r;
        return EnumEvalOk;
    case BinSub:
        *out = l - r;
        return EnumEvalOk;
    case BinMul:
        *out = l * r;
        return EnumEvalOk;
    case BinDiv:
        if (r == 0)
        {
            return EnumEvalNotConst;
        }

        *out = isUnsigned ? (l / r) : (ConstUInt)((ConstInt)l / (ConstInt)r);
        return EnumEvalOk;
    case BinMod:
        if (r == 0)
        {
            return EnumEvalNotConst;
        }

        *out = isUnsigned ? (l % r) : (ConstUInt)((ConstInt)l % (ConstInt)r);
        return EnumEvalOk;
    case BinBitAnd:
        *out = l & r;
        return EnumEvalOk;
    case BinBitOr:
        *out = l | r;
        return EnumEvalOk;
    case BinBitXor:
        *out = l ^ r;
        return EnumEvalOk;
    case BinShl:
        if (r >= 64)
        {
            return EnumEvalNotConst;
        }

        *out = (isUnsigned && ConstBitWidth(e->lhs) == 32) ? ((l << r) & 0xFFFFFFFFULL) : (l << r);
        return EnumEvalOk;
    case BinShr:
        if (r >= 64)
        {
            return EnumEvalNotConst;
        }

        *out = isUnsigned ? (l >> r) : (ConstUInt)((ConstInt)l >> (ConstInt)r);
        return EnumEvalOk;
    default:
        return EnumEvalNotConst; // comparisons / && / || don't fold to an integer value
    }
}

static EnumEvalStatus EnumEvalIdent(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, ConstUInt* out)
{
    // A bare name: a reference to an EARLIER member of this enum.
    IdentExpr* id = (IdentExpr*)n;
    EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, enumIndex);
    size_t refIndex = 0;

    if (!FindEnumMemberIndex(ed, id->name, &refIndex))
    {
        return EnumEvalNotConst;
    }

    if (refIndex >= memberIndex)
    {
        return EnumEvalForwardRef;
    }

    *out = ((EnumMemberDecl*)VecGet(&ed->members, refIndex))->value;
    return EnumEvalOk;
}

static EnumEvalStatus EnumEvalMember(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, ConstUInt* out)
{
    // `EnumName.Member` — a scoped constant from an already-resolved enum.
    MemberExpr* me = (MemberExpr*)n;

    // `int.max` — a builtin scalar pseudo-property folds to its value
    // (integral only; enums never have float underlyings).
    if (me->baseNode->kind == NodeIdent)
    {
        ConstUInt intVal = 0;
        bool isFloat = false;

        if (ScalarPseudoConst(GetPrimitiveType(((IdentExpr*)me->baseNode)->name), me->member, &intVal, NULL, &isFloat))
        {
            if (isFloat)
            {
                return EnumEvalNotConst;
            }

            *out = intVal;
            return EnumEvalOk;
        }
    }

    if (me->baseNode->kind != NodeIdent)
    {
        return EnumEvalNotConst;
    }

    const char* enumName = ((IdentExpr*)me->baseNode)->name;

    for (size_t i = 0; i < mod->enums.count; i++)
    {
        EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, i);

        if (strcmp(ed->name, enumName) != 0)
        {
            continue;
        }

        size_t refIndex = 0;

        if (!FindEnumMemberIndex(ed, me->member, &refIndex))
        {
            return EnumEvalNotConst;
        }

        if (i > enumIndex || (i == enumIndex && refIndex >= memberIndex))
        {
            return EnumEvalForwardRef;
        }

        *out = ((EnumMemberDecl*)VecGet(&ed->members, refIndex))->value;
        return EnumEvalOk;
    }

    return EnumEvalNotConst;
}

// Folds an enum member value expression to a 64-bit value
static EnumEvalStatus EnumEvalConstExpr(Node* n, Module* mod, size_t enumIndex, size_t memberIndex, bool isUnsigned,
                                        ConstUInt* out)
{
    if (!n)
    {
        return EnumEvalNotConst;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        return EnumEvalIntLiteral(n, out);
    case NodeBoolLiteral:
        return EnumEvalBoolLiteral(n, out);
    case NodeUnary:
        return EnumEvalUnary(n, mod, enumIndex, memberIndex, isUnsigned, out);
    case NodeBinary:
        return EnumEvalBinary(n, mod, enumIndex, memberIndex, isUnsigned, out);
    case NodeCast:
        return EnumEvalConstExpr(((CastExpr*)n)->operand, mod, enumIndex, memberIndex, isUnsigned, out);
    case NodeIdent:
        return EnumEvalIdent(n, mod, enumIndex, memberIndex, out);
    case NodeMember:
        return EnumEvalMember(n, mod, enumIndex, memberIndex, out);
    default:
        return EnumEvalNotConst;
    }
}

// Reports the error for a member whose value expression failed to fold.
static void ReportEnumValueError(DiagnosticEngine* diag, EnumMemberDecl* m, EnumEvalStatus st, const char* underlying)
{
    switch (st)
    {
    case EnumEvalNegForUnsigned:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' may not be negative for unsigned underlying type '%s'",
                     m->name, underlying);
        break;
    case EnumEvalForwardRef:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' value forward-references a member that is not resolved yet",
                     m->name);
        break;
    default:
        DiagErrorFmt(diag, m->base.range, "enum member '%s' value must be a constant expression", m->name);
        break;
    }
}

// Assigns enum member values (sequential from 0, or a folded constant
// expression) and validates every value fits the underlying scalar integral
// type. Emits the enum's underlying type in `registry` (registered as an alias
// earlier).
void ResolveEnums(Module* mod, DiagnosticEngine* diag, const TypeRegistry* registry)
{
    for (size_t i = 0; i < mod->enums.count; i++)
    {
        EnumDecl* ed = (EnumDecl*)VecGet(&mod->enums, i);

        const char* underlying = ed->underlyingType ? ed->underlyingType : "int";
        const char* resolved = TypeRegistryResolveAlias(registry, underlying);

        if (!IsEnumUnderlyingType(resolved))
        {
            DiagErrorFmt(diag, ed->base.range, "enum '%s' underlying type '%s' must be a scalar integral type",
                         ed->name, underlying);
            continue;
        }

        for (size_t j = 0; j < ed->members.count; j++)
        {
            EnumMemberDecl* a = (EnumMemberDecl*)VecGet(&ed->members, j);

            for (size_t k = j + 1; k < ed->members.count; k++)
            {
                EnumMemberDecl* b = (EnumMemberDecl*)VecGet(&ed->members, k);

                if (strcmp(a->name, b->name) == 0)
                {
                    DiagErrorFmt(diag, b->base.range, "duplicate enum member '%s' in '%s'", b->name, ed->name);
                }
            }
        }

        bool isUnsigned = false;
        ConstInt minVal = 0;
        ConstUInt maxVal = 0;
        EnumSignedRange(resolved, &isUnsigned, &minVal, &maxVal);

        if (isUnsigned)
        {
            ConstUInt next = 0;

            for (size_t j = 0; j < ed->members.count; j++)
            {
                EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, j);

                if (m->hasExplicitValue)
                {
                    ConstUInt v = 0;
                    EnumEvalStatus st = EnumEvalConstExpr(m->valueExpr, mod, i, j, true, &v);

                    if (st != EnumEvalOk)
                    {
                        ReportEnumValueError(diag, m, st, underlying);
                        m->hasExplicitValue = false;
                    }
                    else
                    {
                        m->value = v;
                        m->isNegative = false;
                    }
                }

                if (m->hasExplicitValue)
                {
                    if (m->isNegative)
                    {
                        DiagErrorFmt(diag, m->base.range,
                                     "enum member '%s' may not be negative for unsigned underlying type '%s'", m->name,
                                     underlying);
                        continue;
                    }

                    if (m->value > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %" PRIu64 " does not fit in '%s'",
                                     m->name, m->value, underlying);
                        continue;
                    }

                    next = m->value + 1;
                }
                else
                {
                    if (next > maxVal)
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value %" PRIu64 " does not fit in '%s'",
                                     m->name, next, underlying);
                        continue;
                    }

                    m->value = next;
                    next++;
                }
            }
        }
        else
        {
            ConstInt next = 0;

            for (size_t j = 0; j < ed->members.count; j++)
            {
                EnumMemberDecl* m = (EnumMemberDecl*)VecGet(&ed->members, j);
                ConstInt sv = next;

                if (m->hasExplicitValue)
                {
                    ConstUInt v = 0;
                    EnumEvalStatus st = EnumEvalConstExpr(m->valueExpr, mod, i, j, false, &v);

                    if (st != EnumEvalOk)
                    {
                        ReportEnumValueError(diag, m, st, underlying);
                        m->hasExplicitValue = false;
                    }
                    else
                    {
                        m->isNegative = (ConstInt)v < 0;
                        m->value = m->isNegative
                                       ? ((v == (ConstUInt)0x8000000000000000ULL) ? v : (ConstUInt)(-(ConstInt)v))
                                       : v;
                    }
                }

                if (m->hasExplicitValue)
                {
                    if (!EnumSignedValue(m->value, m->isNegative, &sv))
                    {
                        DiagErrorFmt(diag, m->base.range, "enum member '%s' value does not fit in '%s'", m->name,
                                     underlying);
                        continue;
                    }

                    next = sv;
                }

                if (sv < minVal || sv > (ConstInt)maxVal)
                {
                    DiagErrorFmt(diag, m->base.range, "enum member '%s' value %" PRId64 " does not fit in '%s'",
                                 m->name, sv, underlying);
                    continue;
                }

                m->value = (ConstUInt)sv;
                next = sv + 1;
            }
        }
    }
}
