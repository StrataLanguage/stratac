#include "Sema/MoveCheck.h"

#include "Codegen/TypeUtil.h"
#include "Sema/Cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* State of one place (`a`, `a.b`, `a.b.c`) at a program point. A place is unusable when it or an
   ancestor is Moved or Maybe; using it whole is also unusable when a part of it is. */
enum {
    StateUnvisited = 0, /* no path has reached this block yet */
    StateLive = 1,
    StateMoved = 2,
    StateMaybe = 3,
};

typedef struct {
    const char* name;
    bool owning;
    const TypeName* type;
} Local;

typedef struct {
    Local* locals;
    size_t count;
    size_t cap;
} Locals;

typedef struct {
    const TypeRegistry* reg;
    const NodeTypes* types;
    Arena* arena;
    Locals locals;
    char** paths;
    size_t pathCount;
    size_t pathCap;
} Env;

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

static bool TypeOwns(const Env* env, const TypeName* t)
{
    /* `rc<T>` is out of scope (being removed): copying a handle retains it, so it never moves. */
    return t && t->name && !t->isRc && TypeClassify(env->reg, env->arena, t).owning;
}

static void AddLocal(Env* env, const char* name, bool owning, const TypeName* type)
{
    Locals* ls = &env->locals;
    int existing = FindLocal(ls, name);

    if (existing >= 0)
    {
        /* A name declared twice (separate scopes) counts as owning if either is. */
        ls->locals[existing].owning |= owning;

        if (owning)
        {
            ls->locals[existing].type = type;
        }

        return;
    }

    if (ls->count == ls->cap)
    {
        ls->cap = ls->cap ? ls->cap * 2 : 8;
        ls->locals = (Local*)realloc(ls->locals, ls->cap * sizeof(Local));
    }

    ls->locals[ls->count++] = (Local){name, owning, type};
}

static const TypeName* DeclType(const Env* env, const VarDeclStmt* vd)
{
    if (vd->type.name && vd->type.name[0])
    {
        return &vd->type;
    }

    return NodeTypesGet(env->types, vd->init);
}

/* The path of a variable-and-fields place into `buf`, and the variable's name; false for anything else. */
static bool PlaceOf(const Node* n, char* buf, size_t cap, const char** root)
{
    if (n->kind == NodeIdent)
    {
        const char* name = ((const IdentExpr*)n)->name;

        if (strlen(name) >= cap)
        {
            return false;
        }

        strcpy(buf, name);
        *root = name;
        return true;
    }

    if (n->kind != NodeMember)
    {
        return false;
    }

    const MemberExpr* m = (const MemberExpr*)n;

    if (m->isImplProperty || m->isEnumConst || m->isScalarConst || !PlaceOf(m->base_node, buf, cap, root))
    {
        return false;
    }

    size_t len = strlen(buf);

    if (len + 1 + strlen(m->member) >= cap)
    {
        return false;
    }

    buf[len] = '.';
    strcpy(buf + len + 1, m->member);
    return true;
}

/* A tracked place: rooted at an owning local. */
static bool TrackedPlace(const Env* env, const Node* n, char* buf, size_t cap)
{
    const char* root;

    if (!PlaceOf(n, buf, cap, &root))
    {
        return false;
    }

    int k = FindLocal(&env->locals, root);
    return k >= 0 && env->locals.locals[k].owning;
}

static int FindPath(const Env* env, const char* path)
{
    for (size_t i = 0; i < env->pathCount; i++)
    {
        if (strcmp(env->paths[i], path) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static void AddPath(Env* env, const char* path)
{
    if (FindPath(env, path) >= 0)
    {
        return;
    }

    if (env->pathCount == env->pathCap)
    {
        env->pathCap = env->pathCap ? env->pathCap * 2 : 16;
        env->paths = (char**)realloc(env->paths, env->pathCap * sizeof(char*));
    }

    env->paths[env->pathCount++] = strdup(path);
}

/* Is `a` a proper ancestor of `d` (`a` is a prefix of `d` ending at a dot)? */
static bool IsAncestor(const char* a, const char* d)
{
    size_t n = strlen(a);
    return strncmp(a, d, n) == 0 && d[n] == '.';
}

/* The declared type of the field `name` of the struct `t` (looking through `^`, `?` and aliases). */
static const TypeName* FieldType(const Env* env, const TypeName* t, const char* name)
{
    while (t && (t->isBox || t->isOptional) && t->inner)
    {
        t = t->inner;
    }

    if (!t || !t->name)
    {
        return NULL;
    }

    const StructType* st = TypeRegistryFind(env->reg, TypeRegistryResolveAlias(env->reg, t->name));

    if (!st || st->opaque)
    {
        return NULL;
    }

    for (size_t i = 0; i < st->fields.count; i++)
    {
        const FieldDecl* f = (const FieldDecl*)VecGet(&st->fields, i);

        if (strcmp(f->name, name) == 0)
        {
            return &f->type;
        }
    }

    return NULL;
}

/* The type of a place node: what sema inferred, else walked from the variable's declared type. */
static const TypeName* PlaceType(const Env* env, const Node* n)
{
    const TypeName* t = NodeTypesGet(env->types, n);

    if (t)
    {
        return t;
    }

    if (n->kind == NodeIdent)
    {
        int k = FindLocal(&env->locals, ((const IdentExpr*)n)->name);
        return k >= 0 ? env->locals.locals[k].type : NULL;
    }

    if (n->kind == NodeMember)
    {
        const MemberExpr* m = (const MemberExpr*)n;
        return FieldType(env, PlaceType(env, m->base_node), m->member);
    }

    return NULL;
}

static void CollectLocals(Env* env, const FunctionDecl* fn, const Cfg* g)
{
    for (size_t i = 0; i < fn->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet(&fn->params, i);

        if (p->mod == ModNone && !p->inferredBorrow && !fn->isExtern)
        {
            AddLocal(env, p->name, TypeOwns(env, &p->type), &p->type);
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
                const TypeName* t = DeclType(env, vd);
                AddLocal(env, vd->name, !vd->isRef && TypeOwns(env, t), t);
            }
        }
    }
}

/* Registers every place the function's ops touch. */
static void CollectPaths(Env* env, const Cfg* g)
{
    char buf[256];

    for (size_t b = 0; b < g->count; b++)
    {
        for (size_t j = 0; j < g->blocks[b].opCount; j++)
        {
            const CfgOp* op = &g->blocks[b].ops[j];

            if (op->kind == CfgDeclare)
            {
                AddPath(env, ((const VarDeclStmt*)op->node)->name);
            }
            else if (op->kind == CfgEval && (op->node->kind == NodeIdent || op->node->kind == NodeMember)
                     && TrackedPlace(env, op->node, buf, sizeof(buf)))
            {
                AddPath(env, buf);
            }
            else if (op->kind == CfgEval && op->node->kind == NodeAssign
                     && TrackedPlace(env, ((const AssignExpr*)op->node)->target, buf, sizeof(buf)))
            {
                AddPath(env, buf);
            }
        }
    }

    /* Every ancestor of a place is a place too, so a move of `a.b` is visible from `a`. */
    size_t known = env->pathCount;

    for (size_t i = 0; i < known; i++)
    {
        strcpy(buf, env->paths[i]);

        for (char* dot = strrchr(buf, '.'); dot; dot = strrchr(buf, '.'))
        {
            *dot = 0;
            AddPath(env, buf);
        }
    }
}

typedef struct {
    MoveDiag* items;
    size_t count;
    size_t cap;
} Found;

static void Report(Found* f, SourceRange range, const char* name)
{
    for (size_t i = 0; i < f->count; i++)
    {
        if (f->items[i].range.start == range.start && f->items[i].range.fileId == range.fileId)
        {
            return;
        }
    }

    if (f->count == f->cap)
    {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->items = (MoveDiag*)realloc(f->items, f->cap * sizeof(MoveDiag));
    }

    f->items[f->count++] = (MoveDiag){range, name};
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

/* A place can be read or moved when neither it nor an ancestor has moved. A part that has moved only
   matters when the whole is used: reading `a.n` is fine while `a.r` is gone. */
static bool PlaceUsable(const Env* env, const unsigned char* state, const char* path, bool wholeUse)
{
    for (size_t i = 0; i < env->pathCount; i++)
    {
        if (state[i] == StateLive)
        {
            continue;
        }

        if (strcmp(env->paths[i], path) == 0 || IsAncestor(env->paths[i], path)
            || (wholeUse && IsAncestor(path, env->paths[i])))
        {
            return false;
        }
    }

    return true;
}

/* Marks a place Moved, or makes it and every part of it Live again. */
static void SetPlace(const Env* env, unsigned char* state, const char* path, unsigned char value)
{
    for (size_t i = 0; i < env->pathCount; i++)
    {
        if (strcmp(env->paths[i], path) == 0 || (value == StateLive && IsAncestor(path, env->paths[i])))
        {
            state[i] = value;
        }
    }
}

static void ReportPlace(Found* found, const Node* node)
{
    const char* root = "";
    char buf[256];
    PlaceOf(node, buf, sizeof(buf), &root);

    /* A member's own range starts at its dot; the use is at the variable. */
    while (node->kind == NodeMember)
    {
        node = ((const MemberExpr*)node)->base_node;
    }

    Report(found, node->range, root);
}

/* Steps one block. With `found` set, uses of unusable places are reported. */
static void Transfer(const Env* env, const CfgBlock* blk, unsigned char* state, Found* found)
{
    char buf[256];

    for (size_t j = 0; j < blk->opCount; j++)
    {
        const CfgOp* op = &blk->ops[j];

        if (op->kind == CfgDeclare)
        {
            SetPlace(env, state, ((const VarDeclStmt*)op->node)->name, StateLive);
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

            if (found && !PlaceUsable(env, state, buf, whole))
            {
                ReportPlace(found, op->node);
            }

            /* Moving out of a `T?` leaves it empty, which is a usable (testable) state; null is its drop flag. */
            const TypeName* placeType = PlaceType(env, op->node);

            if (op->consumed && !(placeType && placeType->isOptional) && TypeOwns(env, placeType))
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

                if (!PlaceUsable(env, state, parent, false))
                {
                    ReportPlace(found, a->target);
                }
            }

            SetPlace(env, state, buf, StateLive);
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

    Env env = {.reg = reg, .types = types, .arena = arena};
    Cfg g = CfgBuild(fn);
    CollectLocals(&env, fn, &g);
    CollectPaths(&env, &g);

    size_t n = env.pathCount;
    Found found = {0};

    if (n > 0)
    {
        /* Entry state of every block; everything is Live on entry. */
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
                Transfer(&env, blk, cur, NULL);

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
                Transfer(&env, &g.blocks[b], cur, &found);
            }
        }

        free(in);
        free(cur);
    }

    for (size_t i = 0; i < env.pathCount; i++)
    {
        free(env.paths[i]);
    }

    free(env.paths);
    free(env.locals.locals);
    CfgFree(&g);
    *out = found.items;
    return found.count;
}

/* The place in `'name' used after move` or `'name' is poisoned: ...`, or false for another message.
   Dotted paths are accepted. */
static bool ParseOldDiag(const char* message, char* name, size_t cap)
{
    if (message[0] != '\'')
    {
        return false;
    }

    const char* close = strchr(message + 1, '\'');

    if (!close)
    {
        return false;
    }

    size_t nameLen = (size_t)(close - (message + 1));

    if (nameLen == 0 || nameLen >= cap || (strcmp(close + 1, " used after move") != 0 && strncmp(close + 1, " is poisoned", 12) != 0))
    {
        return false;
    }

    for (size_t i = 0; i < nameLen; i++)
    {
        char c = message[1 + i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
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
            /* The old checker reports a moved field path at its member's dot, one past the variable. */
            uint32_t slack = strchr(name, '.') ? 1 : 0;
            uint32_t at = all.items[m].range.start;

            if (all.items[m].range.fileId == old->range.fileId && old->range.start >= at && old->range.start <= at + slack)
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
