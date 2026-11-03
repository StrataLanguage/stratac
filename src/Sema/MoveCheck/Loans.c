// Loan conflicts between the operands of one expression.
#include "Sema/MoveCheck/MoveCheckInternal.h"

bool NonTrivial(const Env* env, const Node* n)
{
    const TypeName* t = ValueType(env, n);
    return t && t->name && !TypeClassify(env->reg, env->arena, t).trivial;
}

static bool LoanPlaceOf(const Node* n, char* buf, size_t cap)
{
    const char* root;
    return PlaceKeyImpl(n, buf, cap, &root, true);
}

static void AddAccess(const Env* env, Accesses* acc, const Node* n, LoanMode mode)
{
    char buf[160];

    if (!NonTrivial(env, n) || !LoanPlaceOf(n, buf, sizeof(buf)))
    {
        return;
    }

    if (acc->count == acc->cap)
    {
        acc->cap = acc->cap ? acc->cap * 2 : 8;
        acc->items = (Access*)realloc(acc->items, acc->cap * sizeof(Access));
    }

    Access* a = &acc->items[acc->count++];
    strcpy(a->key, buf);
    a->mode = mode;
    a->node = n;
}

// Could the two places share storage? One is a prefix of the other, index groups matching loosely.
bool PlacesOverlap(const char* a, const char* b)
{
    Seg sa[MaxSegs];
    Seg sb[MaxSegs];
    size_t na = SplitPlace(a, sa);
    size_t nb = SplitPlace(b, sb);

    for (size_t i = 0; i < na && i < nb; i++)
    {
        if (!SegsAlias(sa[i], sb[i]))
        {
            return false;
        }
    }

    return true;
}

LoanMode ArgMode(const CallExpr* e, size_t i)
{
    if (e->isIntrinsicCall)
    {
        if ((strcmp(e->callee, "drop") == 0 && i == 0) || (strcmp(e->callee, "array_push") == 0 && i == 1)
            || (strcmp(e->callee, "array_replace") == 0 && i == 2))
        {
            return LoanMove;
        }

        return i == 0 && IsArrayMutator(e->callee) ? LoanMut : LoanShared;
    }

    const FunctionDecl* d = e->resolvedDecl;
    const ParamDecl* p = d && i < d->params.count ? (const ParamDecl*)VecGet(&d->params, i) : NULL;

    if (!p)
    {
        return LoanShared;
    }

    if (p->mod == ModRef)
    {
        return p->type.isConst ? LoanShared : LoanMut;
    }

    return !d->isExtern && !p->inferredBorrow && !p->isVarargRest ? LoanMove : LoanShared;
}

// Every non-trivial place `n` touches while it is evaluated, with how. `mode` is how `n` itself is taken.
static void CollectAccesses(const Env* env, Accesses* acc, const Node* n, LoanMode mode)
{
    if (!n)
    {
        return;
    }

    char buf[160];

    if ((n->kind == NodeIdent || n->kind == NodeMember || n->kind == NodeIndex) && LoanPlaceOf(n, buf, sizeof(buf)))
    {
        AddAccess(env, acc, n, mode);

        // An index that is itself an expression runs too.
        for (const Node* cur = n; cur && (cur->kind == NodeMember || cur->kind == NodeIndex);)
        {
            if (cur->kind == NodeIndex)
            {
                const IndexExpr* ix = (const IndexExpr*)cur;

                if (ix->index->kind != NodeIdent && ix->index->kind != NodeIntLiteral)
                {
                    CollectAccesses(env, acc, ix->index, LoanShared);
                }

                cur = ix->baseNode;
            }
            else
            {
                cur = ((const MemberExpr*)cur)->baseNode;
            }
        }

        return;
    }

    switch (n->kind)
    {
    case NodeBinary:
        CollectAccesses(env, acc, ((const BinaryExpr*)n)->lhs, LoanShared);
        CollectAccesses(env, acc, ((const BinaryExpr*)n)->rhs, LoanShared);
        break;
    case NodeUnary:
        CollectAccesses(env, acc, ((const UnaryExpr*)n)->operand, LoanShared);
        break;
    case NodeNullTest:
        CollectAccesses(env, acc, ((const NullTestExpr*)n)->operand, LoanShared);
        break;
    case NodeCast:
        CollectAccesses(env, acc, ((const CastExpr*)n)->operand, LoanShared);
        break;
    case NodeMember:
        CollectAccesses(env, acc, ((const MemberExpr*)n)->baseNode, LoanShared);
        break;
    case NodeIndex:
        CollectAccesses(env, acc, ((const IndexExpr*)n)->baseNode, LoanShared);
        CollectAccesses(env, acc, ((const IndexExpr*)n)->index, LoanShared);
        break;
    case NodeConditional:
        CollectAccesses(env, acc, ((const ConditionalExpr*)n)->condition, LoanShared);
        CollectAccesses(env, acc, ((const ConditionalExpr*)n)->thenExpr, mode);
        CollectAccesses(env, acc, ((const ConditionalExpr*)n)->elseExpr, mode);
        break;
    case NodeAssign:
    {
        const AssignExpr* a = (const AssignExpr*)n;
        CollectAccesses(env, acc, a->value, a->op == AssignSet ? LoanMove : LoanShared);
        CollectAccesses(env, acc, a->target, LoanMut);
        break;
    }
    case NodeIncDec:
        CollectAccesses(env, acc, ((const IncDecExpr*)n)->operand, LoanMut);
        break;
    case NodeCall:
    {
        const CallExpr* e = (const CallExpr*)n;
        CollectAccesses(env, acc, e->calleeBase, LoanShared);
        CollectAccesses(env, acc, e->delegateCallee, LoanShared);

        for (size_t i = 0; i < e->args.count; i++)
        {
            CollectAccesses(env, acc, (const Node*)VecGet(&e->args, i), ArgMode(e, i));
        }

        break;
    }
    case NodeStructInit:
    {
        const StructInitExpr* e = (const StructInitExpr*)n;

        for (size_t i = 0; i < e->fields.count; i++)
        {
            CollectAccesses(env, acc, ((const StructInitField*)VecGet(&e->fields, i))->value, LoanMove);
        }

        break;
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* e = (const ArrayInitExpr*)n;

        for (size_t i = 0; i < e->elements.count; i++)
        {
            CollectAccesses(env, acc, (const Node*)VecGet(&e->elements, i), e->repeatCount ? LoanShared : LoanMove);
        }

        break;
    }
    default:
        break;
    }
}

// The operands of `n` in evaluation order, with how each is taken; false when `n` has no such structure.
static bool Operands(const Node* n, const Node** ops, LoanMode* modes, size_t* count, const CallExpr** call)
{
    *call = NULL;

    if (n->kind == NodeBinary && ((const BinaryExpr*)n)->op != BinLogicAnd && ((const BinaryExpr*)n)->op != BinLogicOr)
    {
        ops[0] = ((const BinaryExpr*)n)->lhs;
        ops[1] = ((const BinaryExpr*)n)->rhs;
        modes[0] = modes[1] = LoanShared;
        *count = 2;
        return true;
    }

    if (n->kind == NodeIndex)
    {
        ops[0] = ((const IndexExpr*)n)->baseNode;
        ops[1] = ((const IndexExpr*)n)->index;
        modes[0] = modes[1] = LoanShared;
        *count = 2;
        return true;
    }

    if (n->kind == NodeAssign && ((const AssignExpr*)n)->target->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)((const AssignExpr*)n)->target;
        ops[0] = ix->baseNode;
        ops[1] = ix->index;
        modes[0] = LoanMut;
        modes[1] = LoanShared;
        *count = 2;
        return true;
    }

    return false;
}

static void CheckOperandPair(const Env* env, Found* found, const Node* earlier, LoanMode earlierMode, const Node* later,
                             LoanMode laterMode)
{
    Accesses loans = {0};
    Accesses effects = {0};
    AddAccess(env, &loans, earlier, earlierMode);
    CollectAccesses(env, &effects, later, laterMode);

    for (size_t i = 0; i < loans.count; i++)
    {
        if (loans.items[i].mode == LoanMove)
        {
            continue;
        }

        for (size_t j = 0; j < effects.count; j++)
        {
            bool conflict = loans.items[i].mode == LoanMut || effects.items[j].mode != LoanShared;

            if (conflict && PlacesOverlap(loans.items[i].key, effects.items[j].key))
            {
                ReportAs(found, MoveDiagLoan, effects.items[j].node->range,
                         ArenaStrdup(env->arena, effects.items[j].key));
            }
        }
    }

    free(loans.items);
    free(effects.items);
}

// Checks the loans that the operands of expression `n` take against each other.
void LoanCheck(const Env* env, Found* found, const Node* n)
{
    const Node* ops[MaxSegs];
    LoanMode modes[MaxSegs];
    size_t count = 0;
    const CallExpr* call = NULL;

    if (n->kind == NodeCall)
    {
        call = (const CallExpr*)n;
        count = call->args.count < MaxSegs ? call->args.count : MaxSegs;

        for (size_t i = 0; i < count; i++)
        {
            ops[i] = (const Node*)VecGet(&call->args, i);
            modes[i] = ArgMode(call, i);
        }
    }
    else if (!Operands(n, ops, modes, &count, &call))
    {
        return;
    }

    if (call)
    {
        CheckGlobalArgs(env, found, call);
    }

    for (size_t i = 0; i < count; i++)
    {
        for (size_t j = i + 1; j < count; j++)
        {
            CheckOperandPair(env, found, ops[i], modes[i], ops[j], modes[j]);
        }
    }
}
