#include "Sema/MoveCheck/MoveCheckInternal.h"

static bool WritesAdd(FnWrites* w, const char* key)
{
    for (size_t i = 0; i < w->count; i++)
    {
        if (strcmp(w->keys[i], key) == 0)
        {
            return false;
        }
    }

    if (w->count == w->cap)
    {
        w->cap = w->cap ? w->cap * 2 : 4;
        w->keys = (char**)realloc(w->keys, w->cap * sizeof(char*));
    }

    w->keys[w->count++] = strdup(key);
    return true;
}

static bool ReadsAdd(FnWrites* w, const char* name)
{
    for (size_t i = 0; i < w->readCount; i++)
    {
        if (strcmp(w->reads[i], name) == 0)
        {
            return false;
        }
    }

    if (w->readCount == w->readCap)
    {
        w->readCap = w->readCap ? w->readCap * 2 : 4;
        w->reads = (char**)realloc(w->reads, w->readCap * sizeof(char*));
    }

    w->reads[w->readCount++] = strdup(name);
    return true;
}

static bool IsGlobalPlace(const Env* env, const char* key)
{
    Seg b[MaxSegs];
    char root[128];

    if (SplitPlace(key, b) == 0 || b[0].n >= sizeof(root))
    {
        return false;
    }

    memcpy(root, b[0].p, b[0].n);
    root[b[0].n] = 0;
    return FindLocal(&env->named, root) < 0;
}

static void NoteWrite(const Env* env, FnWrites* w, const Node* place)
{
    char buf[256];
    const char* root;

    if (PlaceKeyImpl(place, buf, sizeof(buf), &root, true) && IsGlobalPlace(env, buf))
    {
        WritesAdd(w, buf);
    }
}

static void NoteCallee(FnWrites* w, const FunctionDecl* d)
{
    if (w->calleeCount == w->calleeCap)
    {
        w->calleeCap = w->calleeCap ? w->calleeCap * 2 : 4;
        w->callees = (const FunctionDecl**)realloc(w->callees, w->calleeCap * sizeof(*w->callees));
    }

    w->callees[w->calleeCount++] = d;
}

/* What `fn` writes by itself, and whom it calls. */
static void ScanWrites(const Env* shared, FnWrites* w, const FunctionDecl* fn)
{
    Env env = {.reg = shared->reg, .types = shared->types, .arena = shared->arena};
    Cfg g = CfgBuild(fn);
    CollectLocals(&env, fn, &g);
    BuildAliases(&env, &g);

    for (size_t b = 0; b < g.count; b++)
    {
        for (size_t j = 0; j < g.blocks[b].opCount; j++)
        {
            const CfgOp* op = &g.blocks[b].ops[j];

            if (op->kind != CfgEval)
            {
                continue;
            }

            if (op->node->kind == NodeIdent || op->node->kind == NodeMember || op->node->kind == NodeIndex)
            {
                const IdentExpr* root = RootIdent(op->node);

                if (root && FindLocal(&env.named, root->name) < 0)
                {
                    ReadsAdd(w, root->name);
                }
            }

            if (op->node->kind == NodeAssign)
            {
                NoteWrite(&env, w, ((const AssignExpr*)op->node)->target);
            }
            else if (op->node->kind == NodeIncDec)
            {
                NoteWrite(&env, w, ((const IncDecExpr*)op->node)->operand);
            }
            else if (op->node->kind == NodeCall)
            {
                const CallExpr* e = (const CallExpr*)op->node;

                for (size_t i = 0; i < e->args.count; i++)
                {
                    LoanMode mode = ArgMode(e, i);

                    if (mode == LoanMut || (e->isIntrinsicCall && strcmp(e->callee, "drop") == 0 && i == 0))
                    {
                        NoteWrite(&env, w, (const Node*)VecGet(&e->args, i));
                    }
                }

                if (e->isIntrinsicCall)
                {
                    continue;
                }

                if (e->resolvedDecl)
                {
                    if (!e->resolvedDecl->isExtern)
                    {
                        NoteCallee(w, e->resolvedDecl);
                    }
                }
                else
                {
                    w->all = true;
                }
            }
        }
    }

    CfgFree(&g);
    FreeAliases(&env);
    free(env.locals.locals);
    free(env.named.locals);
}

WriteSets* WriteSetsBuild(const Module* mod, const TypeRegistry* reg, const NodeTypes* types, Arena* arena)
{
    WriteSets* ws = (WriteSets*)calloc(1, sizeof(WriteSets));
    Env shared = {.reg = reg, .types = types, .arena = arena};
    ws->count = mod->functions.count;
    ws->fns = (FnWrites*)calloc(ws->count ? ws->count : 1, sizeof(FnWrites));

    for (size_t i = 0; i < ws->count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);
        ws->fns[i].fn = fn;

        if (fn->body)
        {
            ScanWrites(&shared, &ws->fns[i], fn);
        }
        else if (!fn->isExtern)
        {
            ws->fns[i].all = true;
        }
    }

    for (bool changed = true; changed;)
    {
        changed = false;

        for (size_t i = 0; i < ws->count; i++)
        {
            FnWrites* w = &ws->fns[i];

            for (size_t c = 0; c < w->calleeCount; c++)
            {
                const FnWrites* callee = FindWrites(ws, w->callees[c]);

                if (!callee)
                {
                    changed |= !w->all;
                    w->all = true;
                    continue;
                }

                if (callee->all && !w->all)
                {
                    w->all = true;
                    changed = true;
                }

                for (size_t k = 0; k < callee->count; k++)
                {
                    changed |= WritesAdd(w, callee->keys[k]);
                }

                for (size_t k = 0; k < callee->readCount; k++)
                {
                    changed |= ReadsAdd(w, callee->reads[k]);
                }
            }
        }
    }

    return ws;
}

void WriteSetsFree(WriteSets* ws)
{
    for (size_t i = 0; ws && i < ws->count; i++)
    {
        for (size_t k = 0; k < ws->fns[i].count; k++)
        {
            free(ws->fns[i].keys[k]);
        }

        for (size_t k = 0; k < ws->fns[i].readCount; k++)
        {
            free(ws->fns[i].reads[k]);
        }

        free(ws->fns[i].keys);
        free(ws->fns[i].reads);
        free(ws->fns[i].callees);
    }

    if (ws)
    {
        free(ws->fns);
        free(ws);
    }
}

/* Does a call to `d` possibly write a place overlapping `key`? Unknown callees may write anything global. */
static bool CalleeWrites(const Env* env, const FunctionDecl* d, const char* key)
{
    const FnWrites* w = FindWrites(env->ws, d);

    if (!w)
    {
        return true;
    }

    for (size_t i = 0; !w->all ? i < w->count : false; i++)
    {
        if (PlacesOverlap(w->keys[i], key))
        {
            return true;
        }
    }

    return w->all;
}

/* An argument that borrows a global place must not be one the callee writes. */
void CheckGlobalArgs(const Env* env, Found* found, const CallExpr* e)
{
    const FunctionDecl* d = e->resolvedDecl;

    if (!env->ws || !d || e->isIntrinsicCall || d->isExtern)
    {
        return;
    }

    for (size_t i = 0; i < e->args.count; i++)
    {
        const Node* arg = (const Node*)VecGet(&e->args, i);
        char buf[256];
        const char* root;

        if (ArgMode(e, i) != LoanMove && NonTrivial(env, arg) && PlaceKeyImpl(arg, buf, sizeof(buf), &root, true)
            && IsGlobalPlace(env, buf) && CalleeWrites(env, d, buf))
        {
            ReportAs(found, MoveDiagGlobalLoan, arg->range, arena_strdup(env->arena, buf));
        }
    }
}

/* A global is built in declaration order, so its initializer may use only the globals before it. */
void TouchGlobal(const InitCtx* c, const char* name, SourceRange range, bool write)
{
    for (size_t j = c->self + 1; j < c->mod->globals.count; j++)
    {
        const GlobalDecl* later = (const GlobalDecl*)VecGet((Vec*)&c->mod->globals, j);

        if (strcmp(later->name, name) == 0)
        {
            const GlobalDecl* me = (const GlobalDecl*)VecGet((Vec*)&c->mod->globals, c->self);

            if (write)
            {
                DiagErrorFmt(c->diag, range,
                             "the initializer of global '%s' changes '%s', which is declared after it and would "
                             "overwrite the change",
                             me->name, name);
            }
            else
            {
                DiagErrorFmt(c->diag, range,
                             "the initializer of global '%s' reads '%s', which is declared after it and not set yet",
                             me->name, name);
            }

            return;
        }
    }
}
