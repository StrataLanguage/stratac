#include "Sema/Cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t breakTarget;
    size_t continueTarget;
    size_t scopeDepth; /* scopes open when the loop began: break and continue leave the ones above */
} LoopFrame;

typedef struct {
    const Block* block;
    Vec defers; /* Vec<const Node*>, in source order */
} ScopeFrame;

typedef struct {
    Cfg* cfg;
    size_t cur;
    ScopeFrame* scopes;
    size_t scopeCount;
    size_t scopeCap;
    LoopFrame* loops;
    size_t loopCount;
    size_t loopCap;
} Builder;

static size_t NewBlock(Builder* b)
{
    Cfg* g = b->cfg;

    if (g->count == g->cap)
    {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->blocks = (CfgBlock*)realloc(g->blocks, g->cap * sizeof(CfgBlock));
    }

    CfgBlock* blk = &g->blocks[g->count];
    memset(blk, 0, sizeof(*blk));
    blk->id = g->count;
    return g->count++;
}

static void EmitC(Builder* b, CfgOpKind kind, const Node* node, bool consumed)
{
    CfgBlock* blk = &b->cfg->blocks[b->cur];

    if (blk->opCount == blk->opCap)
    {
        blk->opCap = blk->opCap ? blk->opCap * 2 : 8;
        blk->ops = (CfgOp*)realloc(blk->ops, blk->opCap * sizeof(CfgOp));
    }

    blk->ops[blk->opCount++] = (CfgOp){kind, node, consumed};
}

static void Emit(Builder* b, CfgOpKind kind, const Node* node)
{
    EmitC(b, kind, node, false);
}

static void AddEdge(Builder* b, size_t from, size_t to)
{
    CfgBlock* blk = &b->cfg->blocks[from];

    if (blk->succCount < 2)
    {
        blk->succ[blk->succCount++] = to;
    }
}

/* Ends the current block with a jump and continues in `next`. */
static void Goto(Builder* b, size_t target)
{
    AddEdge(b, b->cur, target);
}

static void LowerStmt(Builder* b, const Node* n);
static void LowerExprC(Builder* b, const Node* n, bool consumed);

/* A cast that only looks at its operand (a buffer or string viewed as text, an array as a pointer). Any
   other cast hands its operand on, so a move through `rc<T> c = box` still moves the box. */
static bool CastViewsOperand(int kind)
{
    return kind == CastBufferToCString || kind == CastBufferToString || kind == CastStringToCString
           || kind == CastFixedToPtr;
}

static void LowerExpr(Builder* b, const Node* n)
{
    LowerExprC(b, n, false);
}

static void PushScope(Builder* b, const Block* block)
{
    if (b->scopeCount == b->scopeCap)
    {
        b->scopeCap = b->scopeCap ? b->scopeCap * 2 : 8;
        b->scopes = (ScopeFrame*)realloc(b->scopes, b->scopeCap * sizeof(ScopeFrame));
    }

    b->scopes[b->scopeCount].block = block;
    VecInit(&b->scopes[b->scopeCount].defers);
    b->scopeCount++;
}

static void PopScope(Builder* b)
{
    b->scopeCount--;
    free(b->scopes[b->scopeCount].defers.items);
}

/* Runs the deferred statements of the scopes above `depth`, innermost scope and latest `defer`
   first, and emits each scope's exit. Does not pop anything: other paths still need them. */
static void RunDefers(Builder* b, size_t depth)
{
    for (size_t s = b->scopeCount; s > depth; s--)
    {
        ScopeFrame* sf = &b->scopes[s - 1];

        for (size_t d = sf->defers.count; d > 0; d--)
        {
            LowerStmt(b, (const Node*)VecGet(&sf->defers, d - 1));
        }

        if (sf->block)
        {
            Emit(b, CfgScopeExit, &sf->block->base);
        }
    }
}

/* After a jump the rest of the block is dead; later statements go to a block nothing enters. */
static void StartDeadBlock(Builder* b)
{
    b->cur = NewBlock(b);
}

/* `a`, `a.b`, `a.b.c`: a variable and fields only, no index or call between. Such a place is one op, so
   the analyses see `a.b` as a use of that path and not as a use of `a`. */
static bool IsPurePlace(const Node* n)
{
    while (n && n->kind == NodeMember)
    {
        n = ((const MemberExpr*)n)->base_node;
    }

    return n && n->kind == NodeIdent;
}

static void LowerPlace(Builder* b, const Node* n);

/* The base of `base.f` or `base[i]` that is written through: a bare variable there is used (a field of a
   moved struct cannot be assigned), unlike a variable that is itself the whole target. */
static void LowerPlaceRoot(Builder* b, const Node* base)
{
    if (base && base->kind == NodeIdent)
    {
        EmitC(b, CfgEval, base, false);
        return;
    }

    LowerPlace(b, base);
}

/* A place being written or borrowed is not read: only its index operands and call bases run. */
static void LowerPlace(Builder* b, const Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeIdent:
        return;
    case NodeMember:
        if (!IsPurePlace(n))
        {
            LowerPlaceRoot(b, ((const MemberExpr*)n)->base_node);
        }

        return;
    case NodeIndex:
        LowerPlaceRoot(b, ((const IndexExpr*)n)->base_node);
        LowerExpr(b, ((const IndexExpr*)n)->index);
        return;
    default:
        LowerExpr(b, n);
        return;
    }
}

/* Lowers `cond` and ends the block on it. Returns the block that was ended. */
static size_t Branch(Builder* b, const Node* cond, size_t onTrue, size_t onFalse)
{
    LowerExpr(b, cond);
    Emit(b, CfgCondition, cond);
    size_t from = b->cur;
    AddEdge(b, from, onTrue);
    AddEdge(b, from, onFalse);
    return from;
}

/* Does argument `i` of the call move its value? By-value params of a defined function do, unless sema
   inferred a borrow; externs and C varargs only borrow. `drop(x)` and `array_push(a, x)` consume. */
static bool ArgIsConsumed(const CallExpr* e, size_t i)
{
    if (e->isIntrinsicCall)
    {
        return (strcmp(e->callee, "drop") == 0 && i == 0) || (strcmp(e->callee, "array_push") == 0 && i == 1)
               || (strcmp(e->callee, "array_replace") == 0 && i == 2);
    }

    const FunctionDecl* d = e->resolvedDecl;

    if (!d || d->isExtern || d->params.count == 0)
    {
        return false;
    }

    /* Arguments past the named parameters feed a typed rest parameter, which takes them by value. */
    const ParamDecl* p = (const ParamDecl*)VecGet(&d->params, i < d->params.count ? i : d->params.count - 1);

    if (i >= d->params.count && !p->isVarargRest)
    {
        return false;
    }

    return p->mod == ModNone && !p->inferredBorrow;
}

static void LowerExprC(Builder* b, const Node* n, bool consumed)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBinary:
    {
        const BinaryExpr* e = (const BinaryExpr*)n;

        if (e->op == BinLogicAnd || e->op == BinLogicOr)
        {
            size_t rhs = NewBlock(b);
            size_t join = NewBlock(b);
            bool isAnd = e->op == BinLogicAnd;

            Branch(b, e->lhs, isAnd ? rhs : join, isAnd ? join : rhs);
            b->cur = rhs;
            LowerExpr(b, e->rhs);
            Goto(b, join);
            b->cur = join;
            EmitC(b, CfgEval, n, consumed);
            return;
        }

        LowerExpr(b, e->lhs);
        LowerExpr(b, e->rhs);
        break;
    }
    case NodeConditional:
    {
        const ConditionalExpr* e = (const ConditionalExpr*)n;
        size_t thenBlock = NewBlock(b);
        size_t elseBlock = NewBlock(b);
        size_t join = NewBlock(b);

        Branch(b, e->condition, thenBlock, elseBlock);
        b->cur = thenBlock;
        LowerExprC(b, e->thenExpr, consumed);
        Goto(b, join);
        b->cur = elseBlock;
        LowerExprC(b, e->elseExpr, consumed);
        Goto(b, join);
        b->cur = join;
        break;
    }
    case NodeAssign:
    {
        const AssignExpr* e = (const AssignExpr*)n;
        LowerExprC(b, e->value, e->op == AssignSet);
        LowerPlace(b, e->target);
        break;
    }
    case NodeIncDec:
        LowerPlace(b, ((const IncDecExpr*)n)->operand);
        break;
    case NodeUnary:
        LowerExpr(b, ((const UnaryExpr*)n)->operand);
        break;
    case NodeCast:
        /* A text cast (`(cstring)s`) views its operand; any other cast passes it on. */
        LowerExprC(b, ((const CastExpr*)n)->operand, consumed && !CastViewsOperand(((const CastExpr*)n)->textKind));
        break;
    case NodeNullTest:
        LowerExpr(b, ((const NullTestExpr*)n)->operand);
        break;
    case NodeMember:
        if (!IsPurePlace(n))
        {
            LowerExpr(b, ((const MemberExpr*)n)->base_node);
        }

        break;
    case NodeIndex:
        LowerExpr(b, ((const IndexExpr*)n)->base_node);
        LowerExpr(b, ((const IndexExpr*)n)->index);
        break;
    case NodeCall:
    {
        const CallExpr* e = (const CallExpr*)n;
        LowerExpr(b, e->calleeBase);
        LowerExpr(b, e->delegateCallee);

        for (size_t i = 0; i < e->args.count; i++)
        {
            LowerExprC(b, (const Node*)VecGet(&e->args, i), ArgIsConsumed(e, i));
        }

        break;
    }
    case NodeStructInit:
    {
        const StructInitExpr* e = (const StructInitExpr*)n;

        for (size_t i = 0; i < e->fields.count; i++)
        {
            const StructInitField* f = (const StructInitField*)VecGet(&e->fields, i);
            LowerExprC(b, f->value, true);
        }

        break;
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* e = (const ArrayInitExpr*)n;

        for (size_t i = 0; i < e->elements.count; i++)
        {
            /* `{ value; n }` copies the one value into each element; a list moves each element in. */
            LowerExprC(b, (const Node*)VecGet(&e->elements, i), e->repeatCount == NULL);
        }

        LowerExpr(b, e->repeatCount);
        break;
    }
    default:
        break;
    }

    EmitC(b, CfgEval, n, consumed);
}

static void LowerBlock(Builder* b, const Block* blk)
{
    PushScope(b, blk);

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        LowerStmt(b, (const Node*)VecGet(&blk->statements, i));
    }

    /* Falling off the end: the scope's defers, then its exit. */
    ScopeFrame* sf = &b->scopes[b->scopeCount - 1];

    for (size_t d = sf->defers.count; d > 0; d--)
    {
        LowerStmt(b, (const Node*)VecGet(&sf->defers, d - 1));
    }

    Emit(b, CfgScopeExit, &blk->base);
    PopScope(b);
}

static void LowerStmt(Builder* b, const Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        LowerBlock(b, (const Block*)n);
        break;
    case NodeVarDecl:
        LowerExprC(b, ((const VarDeclStmt*)n)->init, !((const VarDeclStmt*)n)->isRef);
        Emit(b, CfgDeclare, n);
        break;
    case NodeExprStmt:
        LowerExpr(b, ((const ExprStmt*)n)->expr);

        /* A `@noreturn` call never continues: nothing runs after it, not even the defers. */
        if (StmtIsNoReturnCall(n))
        {
            StartDeadBlock(b);
        }

        break;
    case NodeDefer:
        if (b->scopeCount > 0)
        {
            VecPush(&b->scopes[b->scopeCount - 1].defers, ((DeferStmt*)n)->stmt);
        }

        break;
    case NodeReturn:
        LowerExprC(b, ((const ReturnStmt*)n)->value, true);
        Emit(b, CfgReturn, n);
        RunDefers(b, 0);
        Goto(b, b->cfg->exit);
        StartDeadBlock(b);
        break;
    case NodeBreak:
    case NodeContinue:
        if (b->loopCount > 0)
        {
            LoopFrame* lf = &b->loops[b->loopCount - 1];
            RunDefers(b, lf->scopeDepth);
            Goto(b, n->kind == NodeBreak ? lf->breakTarget : lf->continueTarget);
            StartDeadBlock(b);
        }

        break;
    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        size_t thenBlock = NewBlock(b);
        size_t elseBlock = s->elseBranch ? NewBlock(b) : 0;
        size_t join = NewBlock(b);

        Branch(b, s->condition, thenBlock, s->elseBranch ? elseBlock : join);
        b->cur = thenBlock;
        LowerStmt(b, s->thenBranch);
        Goto(b, join);

        if (s->elseBranch)
        {
            b->cur = elseBlock;
            LowerStmt(b, s->elseBranch);
            Goto(b, join);
        }

        b->cur = join;
        break;
    }
    case NodeWhile:
    {
        const WhileStmt* s = (const WhileStmt*)n;
        size_t head = NewBlock(b);
        size_t body = NewBlock(b);
        size_t after = NewBlock(b);

        Goto(b, head);
        b->cur = head;

        /* `while (true)` has no way out but `break` and `return`. */
        if (s->condition->kind == NodeBoolLiteral && ((const BoolLiteral*)s->condition)->value)
        {
            Goto(b, body);
        }
        else
        {
            size_t ended = Branch(b, s->condition, body, after);
            b->cfg->blocks[ended].loopCond = true;
        }

        if (b->loopCount == b->loopCap)
        {
            b->loopCap = b->loopCap ? b->loopCap * 2 : 4;
            b->loops = (LoopFrame*)realloc(b->loops, b->loopCap * sizeof(LoopFrame));
        }

        b->loops[b->loopCount++] = (LoopFrame){after, head, b->scopeCount};
        b->cur = body;
        LowerStmt(b, s->body);
        Goto(b, head);
        b->loopCount--;
        b->cur = after;
        break;
    }
    case NodeFor:
    {
        const ForStmt* s = (const ForStmt*)n;
        PushScope(b, NULL); /* the init's variable lives for the whole loop */
        LowerStmt(b, s->init);

        size_t head = NewBlock(b);
        size_t body = NewBlock(b);
        size_t step = NewBlock(b);
        size_t after = NewBlock(b);

        Goto(b, head);
        b->cur = head;

        if (s->condition)
        {
            size_t ended = Branch(b, s->condition, body, after);
            b->cfg->blocks[ended].loopCond = true;
        }
        else
        {
            Goto(b, body);
        }

        if (b->loopCount == b->loopCap)
        {
            b->loopCap = b->loopCap ? b->loopCap * 2 : 4;
            b->loops = (LoopFrame*)realloc(b->loops, b->loopCap * sizeof(LoopFrame));
        }

        b->loops[b->loopCount++] = (LoopFrame){after, step, b->scopeCount};
        b->cur = body;
        LowerStmt(b, s->body);
        Goto(b, step);
        b->loopCount--;
        b->cur = step;
        LowerExpr(b, s->update);
        Goto(b, head);
        b->cur = after;
        PopScope(b);
        break;
    }
    default:
        break;
    }
}

static void MarkReachable(Cfg* g)
{
    size_t* stack = (size_t*)malloc((g->count + 1) * sizeof(size_t) * 2);
    size_t top = 0;
    stack[top++] = g->entry;
    g->blocks[g->entry].reachable = true;

    while (top > 0)
    {
        CfgBlock* blk = &g->blocks[stack[--top]];

        for (unsigned i = 0; i < blk->succCount; i++)
        {
            CfgBlock* next = &g->blocks[blk->succ[i]];

            if (!next->reachable)
            {
                next->reachable = true;
                stack[top++] = next->id;
            }
        }
    }

    free(stack);
}

Cfg CfgBuild(const FunctionDecl* fn)
{
    Cfg g = {0};
    Builder b = {.cfg = &g};

    g.entry = NewBlock(&b);
    g.exit = NewBlock(&b);
    b.cur = g.entry;

    if (fn && fn->body)
    {
        LowerStmt(&b, fn->body);
    }

    Goto(&b, g.exit);
    MarkReachable(&g);

    free(b.scopes);
    free(b.loops);
    return g;
}

void CfgFree(Cfg* cfg)
{
    for (size_t i = 0; i < cfg->count; i++)
    {
        free(cfg->blocks[i].ops);
    }

    free(cfg->blocks);
    *cfg = (Cfg){0};
}

static const char* OpName(const CfgOp* op, char* buf, size_t cap)
{
    switch (op->kind)
    {
    case CfgDeclare:
        snprintf(buf, cap, "declare(%s)", ((const VarDeclStmt*)op->node)->name);
        return buf;
    case CfgCondition:
        return "cond";
    case CfgReturn:
        return "return";
    case CfgScopeExit:
        return "scope-exit";
    case CfgEval:
        break;
    }

    switch (op->node->kind)
    {
    case NodeIdent:
        snprintf(buf, cap, "ident(%s)", ((const IdentExpr*)op->node)->name);
        return buf;
    case NodeCall:
        snprintf(buf, cap, "call(%s)", ((const CallExpr*)op->node)->callee);
        return buf;
    case NodeMember:
        snprintf(buf, cap, "member(%s)", ((const MemberExpr*)op->node)->member);
        return buf;
    case NodeAssign:
        return "assign";
    case NodeIndex:
        return "index";
    case NodeBinary:
        return "binary";
    case NodeConditional:
        return "ternary";
    case NodeIntLiteral:
        return "int";
    case NodeStructInit:
        return "struct-init";
    case NodeArrayInit:
        return "array-init";
    case NodeNullTest:
        return "null-test";
    case NodeCast:
        return "cast";
    case NodeUnary:
        return "unary";
    case NodeIncDec:
        return "incdec";
    default:
        return "expr";
    }
}

char* CfgDump(const Cfg* cfg)
{
    Sb sb;
    SbInit(&sb);

    for (size_t i = 0; i < cfg->count; i++)
    {
        const CfgBlock* blk = &cfg->blocks[i];
        SbPrintf(&sb, "bb%zu:", blk->id);

        for (size_t j = 0; j < blk->opCount; j++)
        {
            char buf[96];
            SbPrintf(&sb, " %s", OpName(&blk->ops[j], buf, sizeof(buf)));
        }

        for (unsigned s = 0; s < blk->succCount; s++)
        {
            SbPrintf(&sb, s == 0 ? " -> bb%zu" : ", bb%zu", blk->succ[s]);
        }

        SbPrintf(&sb, "\n");
    }

    char* out = (char*)malloc(sb.len + 1);
    memcpy(out, sb.data, sb.len);
    out[sb.len] = 0;
    SbFree(&sb);
    return out;
}
