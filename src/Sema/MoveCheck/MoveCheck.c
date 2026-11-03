#include "Sema/MoveCheck/MoveCheckInternal.h"

MoveDiag* ReportAs(Found* f, MoveDiagKind kind, SourceRange range, const char* name)
{
    for (size_t i = 0; i < f->count; i++)
    {
        if (f->items[i].range.start == range.start && f->items[i].range.fileId == range.fileId)
        {
            return NULL;
        }
    }

    if (f->count == f->cap)
    {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->items = (MoveDiag*)realloc(f->items, f->cap * sizeof(MoveDiag));
    }

    f->items[f->count++] = (MoveDiag){range, name, kind, NULL, NULL};
    return &f->items[f->count - 1];
}

static void ReportPlace(const Env* env, Found* found, const Node* node, int blocker)
{
    const char* root = "";
    char buf[256];
    PlaceOf(node, buf, sizeof(buf), &root);
    const char* blocked = env->paths[blocker];
    bool partMoved = IsAncestor(buf, blocked);

    /* A member's own range starts at its dot; the use is at the variable. */
    while (node->kind == NodeMember)
    {
        node = ((const MemberExpr*)node)->base_node;
    }

    /* A use after move names the moved place; a poisoned whole names itself. */
    MoveDiag* d = ReportAs(found, partMoved ? MoveDiagPoisoned : MoveDiagUse, node->range,
                           ArenaStrdup(env->arena, partMoved ? buf : blocked));

    if (d && partMoved)
    {
        d->detail = ArenaStrdup(env->arena, blocked);
    }
}

/* Moving an Owning value out of an indexed element (or a field of one) is forbidden: it would leave a
   hole in the array. An optional element is fine, null being its drop flag. */
static void ElementMoveCheck(const Env* env, Found* found, const CfgOp* op)
{
    const Node* n = op->node;

    if (!op->consumed || IsCopied(env, n) || (n->kind != NodeIndex && n->kind != NodeMember))
    {
        return;
    }

    const TypeName* t = ValueType(env, n);

    if (!t || TypeNameIsOptional(t) || !TypeOwns(env, t))
    {
        return;
    }

    for (const Node* cur = n; cur && (cur->kind == NodeIndex || cur->kind == NodeMember);)
    {
        if (cur->kind == NodeIndex)
        {
            char buf[256];
            const char* root;

            /* An element of a temporary (`Make()[0]`) goes away with it; nothing is left to hole. */
            if (!PlaceKeyImpl(((const IndexExpr*)cur)->base_node, buf, sizeof(buf), &root, true))
            {
                return;
            }

            ReportAs(found, MoveDiagElement, n->range,
                     PlaceKeyImpl(n, buf, sizeof(buf), &root, true) ? ArenaStrdup(env->arena, buf) : "<element>");
            return;
        }

        cur = ((const MemberExpr*)cur)->base_node;
    }
}

/* A value with a `Drop` method is moved whole or not at all: `Drop` expects every field to be there. */
static void DropFieldMoveCheck(const Env* env, Found* found, const CfgOp* op)
{
    const Node* n = op->node;

    if (!op->consumed || IsCopied(env, n) || n->kind != NodeMember || env->inDropMethod)
    {
        return;
    }

    const TypeName* t = ValueType(env, n);

    if (!t || TypeNameIsOptional(t) || !TypeOwns(env, t))
    {
        return;
    }

    for (const Node* cur = n; cur && cur->kind == NodeMember; cur = ((const MemberExpr*)cur)->base_node)
    {
        const TypeName* bt = ValueType(env, ((const MemberExpr*)cur)->base_node);

        while (bt && TypeNameIsBox(bt) && bt->inner)
        {
            bt = bt->inner;
        }

        const StructType* st = bt && bt->name && !TypeNameHasArrayShape(bt)
                                   ? TypeRegistryFind(env->reg, TypeRegistryResolveAlias(env->reg, bt->name))
                                   : NULL;

        if (st && !st->opaque && st->dropFunction)
        {
            char buf[256];
            const char* root;
            ReportAs(found, MoveDiagDropField, n->range,
                     PlaceKeyImpl(n, buf, sizeof(buf), &root, true) ? ArenaStrdup(env->arena, buf) : "<field>");
            return;
        }
    }
}

/* Records on a place expression what its use does with the value, for the code generator: moved out (an Owning
   value, or an optional it empties), copied (a box handed to a bare `T`, a bare struct element), or just read. */
static void AnnotateUse(const Env* env, const CfgOp* op)
{
    const Node* n = op->node;

    if (n->kind != NodeIdent && n->kind != NodeMember && n->kind != NodeIndex)
    {
        return;
    }

    /* Sema does not always infer the type of an identifier (one directly inside a literal): use its declaration. */
    const TypeName* t = ValueType(env, n);

    if (!t)
    {
        t = PlaceType(env, n);
    }

    /* An element of an array whose type is known has the array's element type. */
    if (!t && n->kind == NodeIndex)
    {
        const Node* base = ((const IndexExpr*)n)->base_node;
        const TypeName* bt = base ? ValueType(env, base) : NULL;
        t = bt ? TypeNameArrayElem(bt) : NULL;
    }

    UseKind kind = UseRead;

    if (op->consumed)
    {
        if (IsCopied(env, n))
        {
            kind = UseCopy;
        }
        else if (t && TypeNameIsFixedArray(t))
        {
            /* Inline storage is copied. */
            kind = UseCopy;
        }
        else if (t && TypeOwns(env, t))
        {
            kind = UseMove;
        }
    }

    /* A fixed array handed on as text (`(string)buffer`) is copied even though the graph sees it as a view. */
    if (kind == UseRead && t && TypeNameIsFixedArray(t))
    {
        kind = UseCopy;
    }

    /* The graph is read-only for the checker; the annotation is the one thing it hands back. */
    if (n->kind == NodeIdent)
    {
        ((IdentExpr*)n)->useKind = kind;
    }
    else if (n->kind == NodeMember)
    {
        ((MemberExpr*)n)->useKind = kind;
    }
    else
    {
        ((IndexExpr*)n)->useKind = kind;
    }
}

/* Steps one block. With `found` set, uses of unusable places are reported. */
static void Transfer(const Env* env, const CfgBlock* blk, unsigned char* state, Found* found)
{
    char buf[256];

    for (size_t j = 0; j < blk->opCount; j++)
    {
        const CfgOp* op = &blk->ops[j];

        FactStep(env, state + env->pathCount, op, found);

        if (found && op->kind == CfgEval)
        {
            AnnotateUse(env, op);
            DanglingCheck(env, state + env->pathCount, found, op->node);
            LoanCheck(env, found, op->node);
            ElementMoveCheck(env, found, op);
            DropFieldMoveCheck(env, found, op);
        }

        if (op->kind == CfgDeclare)
        {
            const VarDeclStmt* vd = (const VarDeclStmt*)op->node;

            /* A ref sees the whole place, so it cannot be bound to one with a part moved out. */
            if (found && vd->isRef && vd->init && TrackedPlace(env, vd->init, buf, sizeof(buf)))
            {
                int blocker = PlaceBlocker(env, state, buf, true);

                if (blocker >= 0)
                {
                    ReportPlace(env, found, vd->init, blocker);
                }
            }

            SetPlace(env, state, vd->name, StateLive);
            continue;
        }

        if (op->kind != CfgEval)
        {
            continue;
        }

        if (op->node->kind == NodeIdent || op->node->kind == NodeMember)
        {
            if (!TrackedPlace(env, op->node, buf, sizeof(buf)))
            {
                continue;
            }

            /* A bare variable is used whole; a field read or move uses only that part (and a move
               takes everything under it). */
            bool whole = op->node->kind == NodeIdent || op->consumed;

            int blocker = found ? PlaceBlocker(env, state, buf, whole) : -1;

            if (blocker >= 0)
            {
                ReportPlace(env, found, op->node, blocker);
            }

            /* Moving out of a `T?` leaves it empty, which is a usable (testable) state; null is its drop flag. */
            const TypeName* placeType = PlaceType(env, op->node);

            if (op->consumed && !IsCopied(env, op->node) && !(placeType && TypeNameIsOptional(placeType))
                && TypeOwns(env, placeType))
            {
                SetPlace(env, state, buf, StateMoved);
            }
        }
        else if (op->node->kind == NodeAssign)
        {
            const AssignExpr* a = (const AssignExpr*)op->node;

            if (a->op != AssignSet || !TrackedPlace(env, a->target, buf, sizeof(buf)))
            {
                continue;
            }

            /* Writing a part needs the whole to be in one piece; writing the whole revives it. */
            char* dot = strrchr(buf, '.');

            if (found && dot)
            {
                char parent[256];
                size_t n = (size_t)(dot - buf);
                memcpy(parent, buf, n);
                parent[n] = 0;

                int blocker = PlaceBlocker(env, state, parent, false);

                if (blocker >= 0)
                {
                    ReportPlace(env, found, a->target, blocker);
                }
            }

            SetPlace(env, state, buf, StateLive);
        }
    }
}

static bool IsDropMethod(const FunctionDecl* fn)
{
    size_t n = strlen(fn->name);
    return n >= 4 && strcmp(fn->name + n - 4, "Drop") == 0;
}

static void CollectFunctionFacts(Env* env, const FunctionDecl* fn, Cfg* g)
{
    CollectLocals(env, fn, g);
    BuildAliases(env, g);
    CollectCopies(env, g);
    CollectPaths(env, g);
    CollectFacts(env, g);
}

static unsigned char* SolveEntryStates(Env* env, Cfg* g, size_t n, size_t pathCount)
{
    /* Entry state of every block; everything is Live on entry. */
    unsigned char* in = (unsigned char*)calloc(g->count * n + 1, 1);
    unsigned char* cur = (unsigned char*)malloc(n + 1);
    unsigned char* edge = (unsigned char*)malloc(n + 1);

    for (size_t k = 0; k < n; k++)
    {
        in[g->entry * n + k] = StateLive;
    }

    for (int pass = 0; pass < 64; pass++)
    {
        bool changed = false;

        for (size_t b = 0; b < g->count; b++)
        {
            const CfgBlock* blk = &g->blocks[b];

            if (!blk->reachable)
            {
                continue;
            }

            memcpy(cur, &in[b * n], n);
            Transfer(env, blk, cur, NULL);

            for (unsigned s = 0; s < blk->succCount; s++)
            {
                unsigned char* dst = &in[blk->succ[s] * n];
                const CfgOp* last = blk->opCount ? &blk->ops[blk->opCount - 1] : NULL;

                memcpy(edge, cur, n);

                if (last && last->kind == CfgCondition)
                {
                    ApplyCond(env, edge + pathCount, last->node, s == 0, blk->loopCond && s == 1);
                }

                for (size_t k = 0; k < n; k++)
                {
                    unsigned char joined = Join(dst[k], edge[k]);

                    if (joined != dst[k])
                    {
                        dst[k] = joined;
                        changed = true;
                    }
                }
            }
        }

        if (!changed)
        {
            break;
        }
    }

    free(cur);
    free(edge);
    return in;
}

static void ReportMoveFacts(Env* env, Cfg* g, const unsigned char* in, size_t n, Found* found)
{
    unsigned char* cur = (unsigned char*)malloc(n + 1);

    for (size_t b = 0; b < g->count; b++)
    {
        if (g->blocks[b].reachable)
        {
            memcpy(cur, &in[b * n], n);
            Transfer(env, &g->blocks[b], cur, found);
        }
    }

    free(cur);
}

static void CheckRefParamsRestored(Env* env, const FunctionDecl* fn, const Cfg* g, const unsigned char* in, size_t n,
                                   size_t pathCount, Found* found)
{
    /* A ref parameter belongs to the caller: whatever was taken out of it has to be back by the time the
       function returns. */
    /* A `Drop` method is taking its value apart; nothing needs to be put back. */
    if (g->blocks[g->exit].reachable && !env->inDropMethod)
    {
        for (size_t k = 0; k < pathCount; k++)
        {
            unsigned char st = in[g->exit * n + k];
            char root[128];
            const char* dot = strchr(env->paths[k], '.');
            size_t len = dot ? (size_t)(dot - env->paths[k]) : strlen(env->paths[k]);

            if (st == StateLive || st == StateUnvisited || len >= sizeof(root))
            {
                continue;
            }

            memcpy(root, env->paths[k], len);
            root[len] = 0;

            for (size_t i = 0; i < fn->params.count; i++)
            {
                const ParamDecl* prm = (const ParamDecl*)VecGet(&fn->params, i);

                if (prm->mod == ModRef && !prm->type.isConst && strcmp(prm->name, root) == 0)
                {
                    ReportAs(found, MoveDiagRefParam, fn->base.range, ArenaStrdup(env->arena, env->paths[k]));
                }
            }
        }
    }
}

static void FreeMoveEnv(Env* env)
{
    for (size_t i = 0; i < env->pathCount; i++)
    {
        free(env->paths[i]);
    }

    free(env->paths);
    for (size_t i = 0; i < env->factCount; i++)
    {
        free(env->facts[i]);
    }

    free(env->facts);
    free((void*)env->copied);
    FreeAliases(env);
    free(env->locals.locals);
    free(env->named.locals);
}

size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types, const WriteSets* ws,
                         Arena* arena, MoveDiag** out)
{
    *out = NULL;

    if (!fn || !fn->body)
    {
        return 0;
    }

    Env env = {.reg = reg,
               .types = types,
               .arena = arena,
               .fnReturn = &fn->returnType,
               .fn = fn,
               .ws = ws,
               .inDropMethod = IsDropMethod(fn)};
    Cfg g = CfgBuild(fn);
    CollectFunctionFacts(&env, fn, &g);

    size_t pathCount = env.pathCount;
    size_t n = pathCount + env.factCount + env.aliasCount;
    Found found = {0};

    if (g.count > 0)
    {
        unsigned char* in = SolveEntryStates(&env, &g, n, pathCount);

        ReportMoveFacts(&env, &g, in, n, &found);
        CheckRefParamsRestored(&env, fn, &g, in, n, pathCount, &found);
        free(in);
    }

    FreeMoveEnv(&env);
    CfgFree(&g);
    *out = found.items;
    return found.count;
}

static void EmitNewDiag(DiagnosticEngine* diag, const MoveDiag* d)
{
    switch (d->kind)
    {
    case MoveDiagUse:
        DiagErrorFmt(diag, d->range, "'%s' used after move", d->name);
        break;
    case MoveDiagPoisoned:
        DiagErrorFmt(diag, d->range, "'%s' is poisoned: '%s' was moved out of it; put a value back first", d->name,
                     d->detail);
        break;
    case MoveDiagNarrow:
        if (d->detail && strcmp(d->detail, "definitely empty") == 0)
        {
            DiagErrorFmt(diag, d->range, "'%s' is definitely empty ('%s'); it was tested or moved out of", d->name,
                         d->type);
        }
        else if (d->detail)
        {
            DiagErrorFmt(diag, d->range,
                         "'%s' has not been blessed ('%s'); the array index is not a constant or a trackable "
                         "variable - move or copy the element into a local first",
                         d->name, d->type);
        }
        else
        {
            DiagErrorFmt(diag, d->range, "'%s' has not been blessed ('%s'); test it first: if (%s?) { ... }", d->name,
                         d->type, d->name);
        }

        break;
    case MoveDiagLoan:
        DiagErrorFmt(diag, d->range,
                     "'%s' is borrowed by an earlier operand of this expression and cannot be moved or changed here",
                     d->name);
        break;
    case MoveDiagGlobalLoan:
        DiagErrorFmt(diag, d->range, "'%s' is borrowed by this call, but the callee may write it", d->name);
        break;
    case MoveDiagRefParam:
        DiagErrorFmt(diag, d->range,
                     "'%s' was moved out of a ref parameter and not put back; the caller would see it moved", d->name);
        break;
    case MoveDiagDangle:
        DiagErrorFmt(diag, d->range,
                     "'%s' may dangle: what it refers to may have moved, been reallocated or freed since it was "
                     "bound (bind it again after the change)",
                     d->name);
        break;
    case MoveDiagDropField:
        DiagErrorFmt(diag, d->range, "cannot move '%s' out of a type with a Drop method; move the whole value",
                     d->name);
        break;
    case MoveDiagElement:
        DiagErrorFmt(diag, d->range,
                     "cannot move out of '%s': an element stays in its array; take it with array_remove, "
                     "array_swap_remove, array_replace or array_take",
                     d->name);
        break;
    }
}

static void WalkInit(const InitCtx* c, const Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeIdent:
        TouchGlobal(c, ((const IdentExpr*)n)->name, n->range, false);
        break;
    case NodeMember:
        WalkInit(c, ((const MemberExpr*)n)->base_node);
        break;
    case NodeIndex:
        WalkInit(c, ((const IndexExpr*)n)->base_node);
        WalkInit(c, ((const IndexExpr*)n)->index);
        break;
    case NodeBinary:
        WalkInit(c, ((const BinaryExpr*)n)->lhs);
        WalkInit(c, ((const BinaryExpr*)n)->rhs);
        break;
    case NodeUnary:
        WalkInit(c, ((const UnaryExpr*)n)->operand);
        break;
    case NodeCast:
        WalkInit(c, ((const CastExpr*)n)->operand);
        break;
    case NodeNullTest:
        WalkInit(c, ((const NullTestExpr*)n)->operand);
        break;
    case NodeConditional:
        WalkInit(c, ((const ConditionalExpr*)n)->condition);
        WalkInit(c, ((const ConditionalExpr*)n)->thenExpr);
        WalkInit(c, ((const ConditionalExpr*)n)->elseExpr);
        break;
    case NodeStructInit:
    {
        const StructInitExpr* e = (const StructInitExpr*)n;

        for (size_t i = 0; i < e->fields.count; i++)
        {
            WalkInit(c, ((const StructInitField*)VecGet((Vec*)&e->fields, i))->value);
        }

        break;
    }
    case NodeArrayInit:
    {
        const ArrayInitExpr* e = (const ArrayInitExpr*)n;

        for (size_t i = 0; i < e->elements.count; i++)
        {
            WalkInit(c, (const Node*)VecGet((Vec*)&e->elements, i));
        }

        WalkInit(c, e->repeatCount);
        break;
    }
    case NodeCall:
    {
        const CallExpr* e = (const CallExpr*)n;

        for (size_t i = 0; i < e->args.count; i++)
        {
            WalkInit(c, (const Node*)VecGet((Vec*)&e->args, i));
        }

        const FnWrites* w = FindWrites(c->ws, e->resolvedDecl);

        for (size_t i = 0; w && i < w->readCount; i++)
        {
            TouchGlobal(c, w->reads[i], n->range, false);
        }

        for (size_t i = 0; w && i < w->count; i++)
        {
            Seg segs[MaxSegs];
            char root[128];

            if (SplitPlace(w->keys[i], segs) > 0 && segs[0].n < sizeof(root))
            {
                memcpy(root, segs[0].p, segs[0].n);
                root[segs[0].n] = 0;
                TouchGlobal(c, root, n->range, true);
            }
        }

        break;
    }
    default:
        break;
    }
}

void MoveCheckModule(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, DiagnosticEngine* diag,
                     Arena* arena)
{
    WriteSets* ws = WriteSetsBuild(mod, reg, types, arena);

    for (size_t i = 0; i < mod->globals.count; i++)
    {
        const GlobalDecl* gd = (const GlobalDecl*)VecGet((Vec*)&mod->globals, i);
        InitCtx ctx = {mod, ws, diag, i};
        WalkInit(&ctx, gd->init);
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);
        MoveDiag* found = NULL;
        size_t count = MoveCheckFunction(fn, reg, types, ws, arena, &found);

        for (size_t m = 0; m < count; m++)
        {
            EmitNewDiag(diag, &found[m]);
        }

        free(found);
    }

    WriteSetsFree(ws);
}
