#include "Sema/MoveCheck.h"

#include "Codegen/TypeUtil.h"
#include "Sema/Cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    StateUnvisited = 0, /* no path has reached this block yet */
    StateLive = 1,
    StateMoved = 2,
    StateMaybe = 3,
};

typedef struct {
    const char* name;
    bool owning;
} Local;

typedef struct {
    Local* locals;
    size_t count;
    size_t cap;
} Locals;

static int FindLocal(const Locals* ls, const char* name)
{
    for (size_t i = ls->count; i > 0; i--)
    {
        if (strcmp(ls->locals[i - 1].name, name) == 0)
        {
            return (int)(i - 1);
        }
    }

    return -1;
}

static void AddLocal(Locals* ls, const char* name, bool owning)
{
    int existing = FindLocal(ls, name);

    if (existing >= 0)
    {
        /* A name declared twice (separate scopes) counts as owning if either is. */
        ls->locals[existing].owning |= owning;
        return;
    }

    if (ls->count == ls->cap)
    {
        ls->cap = ls->cap ? ls->cap * 2 : 8;
        ls->locals = (Local*)realloc(ls->locals, ls->cap * sizeof(Local));
    }

    ls->locals[ls->count++] = (Local){name, owning};
}

static unsigned char Join(unsigned char a, unsigned char b)
{
    if (a == StateUnvisited)
    {
        return b;
    }

    if (b == StateUnvisited || a == b)
    {
        return a;
    }

    return StateMaybe;
}

typedef struct {
    const TypeRegistry* reg;
    const NodeTypes* types;
    Arena* arena;
} Env;

static bool TypeOwns(const Env* env, const TypeName* t)
{
    /* `rc<T>` is out of scope (being removed): copying a handle retains it, so it never moves. */
    return t && t->name && !t->isRc && TypeClassify(env->reg, env->arena, t).owning;
}

static const TypeName* DeclType(const Env* env, const VarDeclStmt* vd)
{
    if (vd->type.name && vd->type.name[0])
    {
        return &vd->type;
    }

    return NodeTypesGet(env->types, vd->init);
}

static void CollectLocals(const Env* env, const FunctionDecl* fn, const Cfg* g, Locals* ls)
{
    for (size_t i = 0; i < fn->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet(&fn->params, i);

        if (p->mod == ModNone && !p->inferredBorrow && !fn->isExtern)
        {
            AddLocal(ls, p->name, TypeOwns(env, &p->type));
        }
    }

    for (size_t b = 0; b < g->count; b++)
    {
        for (size_t j = 0; j < g->blocks[b].opCount; j++)
        {
            const CfgOp* op = &g->blocks[b].ops[j];

            if (op->kind == CfgDeclare)
            {
                const VarDeclStmt* vd = (const VarDeclStmt*)op->node;
                AddLocal(ls, vd->name, !vd->isRef && TypeOwns(env, DeclType(env, vd)));
            }
        }
    }
}

typedef struct {
    MoveDiag* items;
    size_t count;
    size_t cap;
} Found;

static void Report(Found* f, const IdentExpr* id)
{
    for (size_t i = 0; i < f->count; i++)
    {
        if (f->items[i].range.start == id->base.range.start && f->items[i].range.fileId == id->base.range.fileId)
        {
            return;
        }
    }

    if (f->count == f->cap)
    {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->items = (MoveDiag*)realloc(f->items, f->cap * sizeof(MoveDiag));
    }

    f->items[f->count++] = (MoveDiag){id->base.range, id->name};
}

/* Steps one block. With `found` set, uses of non-Live variables are reported. */
static void Transfer(const CfgBlock* blk, const Locals* ls, unsigned char* state, Found* found)
{
    for (size_t j = 0; j < blk->opCount; j++)
    {
        const CfgOp* op = &blk->ops[j];

        if (op->kind == CfgDeclare)
        {
            int k = FindLocal(ls, ((const VarDeclStmt*)op->node)->name);

            if (k >= 0)
            {
                state[k] = StateLive;
            }

            continue;
        }

        if (op->kind != CfgEval)
        {
            continue;
        }

        if (op->node->kind == NodeIdent)
        {
            const IdentExpr* id = (const IdentExpr*)op->node;
            int k = FindLocal(ls, id->name);

            if (k < 0 || !ls->locals[k].owning)
            {
                continue;
            }

            if (state[k] != StateLive && found)
            {
                Report(found, id);
            }

            if (op->consumed)
            {
                state[k] = StateMoved;
            }
        }
        else if (op->node->kind == NodeAssign)
        {
            const AssignExpr* a = (const AssignExpr*)op->node;

            if (a->op == AssignSet && a->target->kind == NodeIdent)
            {
                int k = FindLocal(ls, ((const IdentExpr*)a->target)->name);

                if (k >= 0)
                {
                    state[k] = StateLive;
                }
            }
        }
    }
}

size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types, Arena* arena,
                         MoveDiag** out)
{
    *out = NULL;

    if (!fn || !fn->body)
    {
        return 0;
    }

    Env env = {reg, types, arena};
    Cfg g = CfgBuild(fn);
    Locals ls = {0};
    CollectLocals(&env, fn, &g, &ls);

    size_t n = ls.count;
    Found found = {0};

    if (n > 0)
    {
        /* Entry state of every block; params are Live on entry. */
        unsigned char* in = (unsigned char*)calloc(g.count * n, 1);
        unsigned char* cur = (unsigned char*)malloc(n);

        for (size_t k = 0; k < n; k++)
        {
            in[g.entry * n + k] = StateLive;
        }

        for (int pass = 0; pass < 64; pass++)
        {
            bool changed = false;

            for (size_t b = 0; b < g.count; b++)
            {
                const CfgBlock* blk = &g.blocks[b];

                if (!blk->reachable)
                {
                    continue;
                }

                memcpy(cur, &in[b * n], n);
                Transfer(blk, &ls, cur, NULL);

                for (unsigned s = 0; s < blk->succCount; s++)
                {
                    unsigned char* dst = &in[blk->succ[s] * n];

                    for (size_t k = 0; k < n; k++)
                    {
                        unsigned char joined = Join(dst[k], cur[k]);

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

        for (size_t b = 0; b < g.count; b++)
        {
            if (g.blocks[b].reachable)
            {
                memcpy(cur, &in[b * n], n);
                Transfer(&g.blocks[b], &ls, cur, &found);
            }
        }

        free(in);
        free(cur);
    }

    free(ls.locals);
    CfgFree(&g);
    *out = found.items;
    return found.count;
}

/* The identifier in `'name' used after move`, or NULL for a path (`'a.b'`, `'a[0]'`) or another message. */
static bool ParseOldDiag(const char* message, char* name, size_t cap)
{
    static const char suffix[] = "' used after move";
    size_t len = strlen(message);

    if (message[0] != '\'' || len < sizeof(suffix) || strcmp(message + len - (sizeof(suffix) - 1), suffix) != 0)
    {
        return false;
    }

    size_t nameLen = len - (sizeof(suffix) - 1) - 1;

    if (nameLen == 0 || nameLen >= cap)
    {
        return false;
    }

    for (size_t i = 0; i < nameLen; i++)
    {
        char c = message[1 + i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return false;
        }
    }

    memcpy(name, message + 1, nameLen);
    name[nameLen] = 0;
    return true;
}

size_t MoveCheckShadow(const Module* mod, const TypeRegistry* reg, const NodeTypes* types,
                       const DiagnosticEngine* diag, Arena* arena)
{
    size_t disagreements = 0;
    Found all = {0};

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);
        MoveDiag* mine = NULL;
        size_t mineCount = MoveCheckFunction(fn, reg, types, arena, &mine);

        for (size_t m = 0; m < mineCount; m++)
        {
            if (all.count == all.cap)
            {
                all.cap = all.cap ? all.cap * 2 : 16;
                all.items = (MoveDiag*)realloc(all.items, all.cap * sizeof(MoveDiag));
            }

            all.items[all.count++] = mine[m];
        }

        free(mine);
    }

    bool* matched = (bool*)calloc(all.count ? all.count : 1, 1);

    for (size_t d = 0; d < diag->m_count; d++)
    {
        const Diagnostic* old = &diag->m_diagnostics[d];
        char name[64];

        if (old->severity != SevError || !ParseOldDiag(old->message, name, sizeof(name)))
        {
            continue;
        }

        bool found = false;

        for (size_t m = 0; m < all.count; m++)
        {
            if (all.items[m].range.start == old->range.start && all.items[m].range.fileId == old->range.fileId)
            {
                matched[m] = true;
                found = true;
            }
        }

        if (!found)
        {
            fprintf(stderr, "shadow: only-old %u:%u '%s'\n", old->range.fileId, old->range.start, name);
            disagreements++;
        }
    }

    for (size_t m = 0; m < all.count; m++)
    {
        if (!matched[m])
        {
            fprintf(stderr, "shadow: only-new %u:%u '%s'\n", all.items[m].range.fileId, all.items[m].range.start,
                    all.items[m].name);
            disagreements++;
        }
    }

    free(matched);
    free(all.items);
    return disagreements;
}
