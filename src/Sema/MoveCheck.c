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
    Locals named; /* every parameter and local, whatever its type; any other root is a global */
    const TypeName* fnReturn;
    bool inDropMethod; /* a Drop method may take its own fields apart */
    const WriteSets* ws;
    char** aliasName; /* `ref` locals and the place each was bound to */
    char** aliasTarget;
    size_t aliasCount;
    size_t aliasCap;
    char** paths;
    size_t pathCount;
    size_t pathCap;
    char** facts; /* optional places that narrowing can prove non-empty */
    size_t factCount;
    size_t factCap;
} Env;

/* The aliases of the function being checked, so that place keys can name the bound place. */
static const Env* g_aliasEnv;

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

static void AddLocalTo(Locals* ls, const char* name, bool owning, const TypeName* type)
{
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

static void AddLocal(Env* env, const char* name, bool owning, const TypeName* type)
{
    AddLocalTo(&env->locals, name, owning, type);
    AddLocalTo(&env->named, name, false, NULL);
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

        AddLocalTo(&env->named, p->name, false, NULL);

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

static bool PlaceKeyImpl(const Node* n, char* buf, size_t cap, const char** root, bool lax);

/* Records, for each `ref x = place;` declaration, the place `x` stands for. A name declared twice with
   different places is ambiguous and stands for nothing. */
static void AddAlias(Env* env, const char* name, const char* target)
{
    for (size_t i = 0; i < env->aliasCount; i++)
    {
        if (strcmp(env->aliasName[i], name) == 0)
        {
            if (strcmp(env->aliasTarget[i], target) != 0)
            {
                env->aliasTarget[i][0] = 0;
            }

            return;
        }
    }

    if (env->aliasCount == env->aliasCap)
    {
        env->aliasCap = env->aliasCap ? env->aliasCap * 2 : 4;
        env->aliasName = (char**)realloc(env->aliasName, env->aliasCap * sizeof(char*));
        env->aliasTarget = (char**)realloc(env->aliasTarget, env->aliasCap * sizeof(char*));
    }

    env->aliasName[env->aliasCount] = strdup(name);
    env->aliasTarget[env->aliasCount++] = strdup(target);
}

static void FreeAliases(Env* env)
{
    for (size_t i = 0; i < env->aliasCount; i++)
    {
        free(env->aliasName[i]);
        free(env->aliasTarget[i]);
    }

    free(env->aliasName);
    free(env->aliasTarget);
    env->aliasName = env->aliasTarget = NULL;
    env->aliasCount = env->aliasCap = 0;
    g_aliasEnv = NULL;
}

static void BuildAliases(Env* env, const Cfg* g)
{
    char buf[256];
    const char* root;
    g_aliasEnv = env;

    for (size_t b = 0; b < g->count; b++)
    {
        for (size_t j = 0; j < g->blocks[b].opCount; j++)
        {
            const CfgOp* op = &g->blocks[b].ops[j];

            if (op->kind == CfgDeclare)
            {
                const VarDeclStmt* vd = (const VarDeclStmt*)op->node;

                if (vd->isRef && vd->init && PlaceKeyImpl(vd->init, buf, sizeof(buf), &root, true))
                {
                    AddAlias(env, vd->name, buf);
                }
            }
        }
    }
}

/* Write sets: the global places each function may write, directly or through the functions it calls. A
   call changes only those, so it ends narrowing only for them, and an argument that borrows one of them
   conflicts with a callee that writes it. `all` is for a callee nothing is known about. */
typedef struct {
    const FunctionDecl* fn;
    char** keys;
    size_t count;
    size_t cap;
    const FunctionDecl** callees;
    size_t calleeCount;
    size_t calleeCap;
    bool all;
} FnWrites;

struct WriteSets {
    FnWrites* fns;
    size_t count;
};

static const FnWrites* FindWrites(const WriteSets* ws, const FunctionDecl* d)
{
    for (size_t i = 0; ws && d && i < ws->count; i++)
    {
        if (ws->fns[i].fn == d)
        {
            return &ws->fns[i];
        }
    }

    return NULL;
}

/* Narrowing: a `T?` place is proven non-empty by a null test, or by being set from a value that is.
   The proof is a must-fact: it holds only if every path to the point agrees. Facts live after the move
   states in a block's state array: 1 = not proven, 2 = non-empty (3 = paths disagree). */
enum {
    FactNone = 1,
    FactNonEmpty = 2,
};

/* The place of a variable-and-fields-and-indices expression (`a`, `a.b`, `a[i].b`, `a[0]`). An index
   is a variable or an integer literal; any other index is no place. */
static const char* AliasTarget(const char* name)
{
    const Env* env = g_aliasEnv;

    for (size_t i = 0; env && i < env->aliasCount; i++)
    {
        if (strcmp(env->aliasName[i], name) == 0)
        {
            return env->aliasTarget[i][0] ? env->aliasTarget[i] : NULL;
        }
    }

    return NULL;
}

static bool PlaceKeyImpl(const Node* n, char* buf, size_t cap, const char** root, bool lax);

static bool FactPlaceOf(const Node* n, char* buf, size_t cap, const char** root)
{
    return PlaceKeyImpl(n, buf, cap, root, false);
}

/* With `lax`, an index that is neither a variable nor a literal is `[?]`, which may be any element. */
static bool PlaceKeyImpl(const Node* n, char* buf, size_t cap, const char** root, bool lax)
{
    if (n->kind == NodeIdent)
    {
        if (!PlaceOf(n, buf, cap, root))
        {
            return false;
        }

        const char* target = AliasTarget(buf);

        if (target)
        {
            if (strlen(target) >= cap)
            {
                return false;
            }

            strcpy(buf, target);
        }

        return true;
    }

    const Node* base;
    char suffix[64];

    if (n->kind == NodeMember)
    {
        const MemberExpr* m = (const MemberExpr*)n;

        if (m->isImplProperty || m->isEnumConst || m->isScalarConst || strlen(m->member) + 2 > sizeof(suffix))
        {
            return false;
        }

        base = m->base_node;
        snprintf(suffix, sizeof(suffix), ".%s", m->member);
    }
    else if (n->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)n;
        base = ix->base_node;

        if (ix->index->kind == NodeIdent && strlen(((const IdentExpr*)ix->index)->name) + 3 <= sizeof(suffix))
        {
            snprintf(suffix, sizeof(suffix), "[%s]", ((const IdentExpr*)ix->index)->name);
        }
        else if (ix->index->kind == NodeIntLiteral)
        {
            snprintf(suffix, sizeof(suffix), "[%llu]", (unsigned long long)((const IntLiteral*)ix->index)->value);
        }
        else if (lax)
        {
            strcpy(suffix, "[?]");
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    if (!PlaceKeyImpl(base, buf, cap, root, lax))
    {
        return false;
    }

    size_t len = strlen(buf);
    size_t add = strlen(suffix);

    if (len + add >= cap)
    {
        return false;
    }

    memcpy(buf + len, suffix, add + 1);
    return true;
}

static int FindFact(const Env* env, const char* path)
{
    for (size_t i = 0; i < env->factCount; i++)
    {
        if (strcmp(env->facts[i], path) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static void AddFact(Env* env, const char* path)
{
    if (FindFact(env, path) >= 0)
    {
        return;
    }

    if (env->factCount == env->factCap)
    {
        env->factCap = env->factCap ? env->factCap * 2 : 16;
        env->facts = (char**)realloc(env->facts, env->factCap * sizeof(char*));
    }

    env->facts[env->factCount++] = strdup(path);
}

/* A place split into segments: names (`a`, `b`) and index groups (`[i]`, `[0]`). */
typedef struct {
    const char* p;
    size_t n;
} Seg;

enum { MaxSegs = 24 };

static size_t SplitPlace(const char* path, Seg* segs)
{
    size_t count = 0;

    for (const char* s = path; *s && count < MaxSegs;)
    {
        if (*s == '.')
        {
            s++;
            continue;
        }

        const char* start = s;

        if (*s == '[')
        {
            while (*s && *s != ']')
            {
                s++;
            }

            s += *s == ']';
        }
        else
        {
            while (*s && *s != '.' && *s != '[')
            {
                s++;
            }
        }

        segs[count++] = (Seg){start, (size_t)(s - start)};
    }

    return count;
}

/* Can the two segments name the same thing? Different literal indices cannot; a variable index can be anything. */
static bool SegsAlias(Seg a, Seg b)
{
    if (a.p[0] == '[' && b.p[0] == '[' && !(a.p[1] >= '0' && a.p[1] <= '9' && b.p[1] >= '0' && b.p[1] <= '9'))
    {
        return true;
    }

    return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

/* The type of a value: what sema inferred, else a call's declared return type. */
static const TypeName* ValueType(const Env* env, const Node* n)
{
    const TypeName* t = NodeTypesGet(env->types, n);

    if (!t && n->kind == NodeCall && ((const CallExpr*)n)->resolvedDecl)
    {
        t = &((const CallExpr*)n)->resolvedDecl->returnType;
    }

    return t;
}

/* Registers every optional place on the chain `n`, `n`'s base, and so on. */
static void AddOptionalChain(Env* env, const Node* n)
{
    char buf[256];
    const char* root;

    while (n)
    {
        const TypeName* t = NodeTypesGet(env->types, n);

        if (t && t->isOptional && FactPlaceOf(n, buf, sizeof(buf), &root))
        {
            AddFact(env, buf);
        }

        if (n->kind == NodeMember)
        {
            n = ((const MemberExpr*)n)->base_node;
        }
        else if (n->kind == NodeIndex)
        {
            n = ((const IndexExpr*)n)->base_node;
        }
        else
        {
            break;
        }
    }
}

static void CollectFacts(Env* env, const Cfg* g)
{
    for (size_t b = 0; b < g->count; b++)
    {
        for (size_t j = 0; j < g->blocks[b].opCount; j++)
        {
            const CfgOp* op = &g->blocks[b].ops[j];

            if (op->kind == CfgDeclare)
            {
                const VarDeclStmt* vd = (const VarDeclStmt*)op->node;
                const TypeName* t = DeclType(env, vd);

                if (t && t->isOptional)
                {
                    AddFact(env, vd->name);
                }
            }
            else if (op->kind == CfgEval
                     && (op->node->kind == NodeIdent || op->node->kind == NodeMember || op->node->kind == NodeIndex))
            {
                AddOptionalChain(env, op->node);
            }
            else if (op->kind == CfgEval && op->node->kind == NodeAssign)
            {
                AddOptionalChain(env, ((const AssignExpr*)op->node)->target);
            }
        }
    }
}

static void SetFact(const Env* env, unsigned char* f, const char* path, unsigned char value)
{
    int k = FindFact(env, path);

    if (k >= 0)
    {
        f[k] = value;
    }
}

/* `key` was written or moved out of: forget what was proven about it, about what is under it, and about
   every place that may be the same (`a[j]` for `a[i]`). With `selfToo` false only what is under it. */
static void KillFacts(const Env* env, unsigned char* f, const char* key, bool selfToo)
{
    Seg a[MaxSegs];
    size_t na = SplitPlace(key, a);

    for (size_t i = 0; i < env->factCount; i++)
    {
        Seg b[MaxSegs];
        size_t nb = SplitPlace(env->facts[i], b);
        bool hit = nb > na || (nb == na && selfToo);

        for (size_t s = 0; hit && s < na; s++)
        {
            hit = SegsAlias(a[s], b[s]);
        }

        if (hit)
        {
            f[i] = FactNone;
        }
    }
}

/* The variable `name` changed: facts about elements indexed by it no longer hold. */
static void KillIndexVar(const Env* env, unsigned char* f, const char* name)
{
    size_t len = strlen(name);

    for (size_t i = 0; i < env->factCount; i++)
    {
        Seg b[MaxSegs];
        size_t nb = SplitPlace(env->facts[i], b);

        for (size_t s = 0; s < nb; s++)
        {
            if (b[s].p[0] == '[' && b[s].n == len + 2 && memcmp(b[s].p + 1, name, len) == 0)
            {
                f[i] = FactNone;
            }
        }
    }
}

/* A call may write any global. */
static void KillGlobals(const Env* env, unsigned char* f)
{
    for (size_t i = 0; i < env->factCount; i++)
    {
        Seg b[MaxSegs];
        char root[128];

        if (SplitPlace(env->facts[i], b) > 0 && b[0].n < sizeof(root))
        {
            memcpy(root, b[0].p, b[0].n);
            root[b[0].n] = 0;

            if (FindLocal(&env->named, root) < 0)
            {
                f[i] = FactNone;
            }
        }
    }
}

/* Applies what `c` evaluating to `whenTrue` proves: `x?` terms joined by `&&` (all hold when true) or
   `||` (all hold when false), under any number of `!`. */
static void ApplyCond(const Env* env, unsigned char* f, const Node* c, bool whenTrue)
{
    while (c && c->kind == NodeUnary && ((const UnaryExpr*)c)->op == UnNot)
    {
        c = ((const UnaryExpr*)c)->operand;
        whenTrue = !whenTrue;
    }

    if (!c)
    {
        return;
    }

    if (c->kind == NodeNullTest)
    {
        char buf[256];
        const char* root;

        if (whenTrue && FactPlaceOf(((const NullTestExpr*)c)->operand, buf, sizeof(buf), &root))
        {
            SetFact(env, f, buf, FactNonEmpty);
        }
    }
    else if (c->kind == NodeBinary)
    {
        const BinaryExpr* b = (const BinaryExpr*)c;

        if ((b->op == BinLogicAnd && whenTrue) || (b->op == BinLogicOr && !whenTrue))
        {
            ApplyCond(env, f, b->lhs, whenTrue);
            ApplyCond(env, f, b->rhs, whenTrue);
        }
    }
}

static const char* PlaceKey(const Env* env, const Node* n)
{
    char buf[256];
    const char* root;
    return FactPlaceOf(n, buf, sizeof(buf), &root) ? arena_strdup(env->arena, buf) : "<expression>";
}

/* Reading through a `T?` needs the proof. */
static void RequireProof(const Env* env, const unsigned char* f, Found* found, const Node* value, SourceRange range)
{
    char buf[256];
    const char* root;
    int k = FactPlaceOf(value, buf, sizeof(buf), &root) ? FindFact(env, buf) : -1;

    if (found && !(k >= 0 && f[k] == FactNonEmpty))
    {
        Report(found, range, PlaceKey(env, value));
    }
}

/* `value` is handed to something of type `target` (NULL: unknown, so no proof is wanted). Returns whether
   the target may count it as non-empty. A moved optional leaves its source empty. */
static bool ConsumeValue(const Env* env, unsigned char* f, Found* found, const Node* value, const TypeName* target,
                         SourceRange range, bool move)
{
    if (!value)
    {
        return false;
    }

    const TypeName* vt = ValueType(env, value);

    if (!vt || !vt->isOptional)
    {
        return true;
    }

    char buf[256];
    const char* root;
    bool haveKey = FactPlaceOf(value, buf, sizeof(buf), &root);
    int k = haveKey ? FindFact(env, buf) : -1;
    bool proven = k >= 0 && f[k] == FactNonEmpty;

    if (target && !target->isOptional && !proven && found)
    {
        Report(found, range, PlaceKey(env, value));
    }

    if (move && haveKey && TypeOwns(env, vt))
    {
        KillFacts(env, f, buf, true);
    }

    return proven;
}

static bool IsArrayMutator(const char* callee)
{
    static const char* const names[] = {"array_pop",     "array_push",   "array_remove",
                                        "array_reserve", "array_resize", "array_swap_remove"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcmp(callee, names[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

static void KillPlaceOf(const Env* env, unsigned char* f, const Node* n, bool selfToo)
{
    char buf[256];
    const char* root;

    if (FactPlaceOf(n, buf, sizeof(buf), &root))
    {
        KillFacts(env, f, buf, selfToo);

        if (n->kind == NodeIdent)
        {
            KillIndexVar(env, f, buf);
        }
    }
}

static void CallFactStep(const Env* env, unsigned char* f, Found* found, const CallExpr* e)
{
    if (e->isIntrinsicCall)
    {
        const Node* arg0 = e->args.count > 0 ? (const Node*)VecGet(&e->args, 0) : NULL;
        const Node* arg1 = e->args.count > 1 ? (const Node*)VecGet(&e->args, 1) : NULL;

        if (arg0 && strcmp(e->callee, "drop") == 0)
        {
            ConsumeValue(env, f, found, arg0, NULL, arg0->range, true);
        }
        else if (arg1 && strcmp(e->callee, "array_push") == 0)
        {
            ConsumeValue(env, f, found, arg1, NULL, arg1->range, true);
        }

        if (arg0 && IsArrayMutator(e->callee))
        {
            KillPlaceOf(env, f, arg0, true);
        }

        return;
    }

    const FunctionDecl* d = e->resolvedDecl;

    for (size_t i = 0; d && i < e->args.count; i++)
    {
        const Node* arg = (const Node*)VecGet(&e->args, i);
        const ParamDecl* p = i < d->params.count ? (const ParamDecl*)VecGet(&d->params, i) : NULL;
        bool byValue = p && !d->isExtern && p->mod == ModNone && !p->inferredBorrow && !p->isVarargRest;

        ConsumeValue(env, f, found, arg, p && !p->isVarargRest ? &p->type : NULL, arg->range, byValue);

        if (p && p->mod == ModRef && !p->type.isConst)
        {
            KillPlaceOf(env, f, arg, true);
        }
    }

    const FnWrites* w = FindWrites(env->ws, d);

    if (!w || w->all)
    {
        KillGlobals(env, f);
        return;
    }

    for (size_t i = 0; i < w->count; i++)
    {
        KillFacts(env, f, w->keys[i], true);
    }
}

/* The effect of one op on the narrowing facts; with `found` set, reads that lack the proof are reported. */
static void FactStep(const Env* env, unsigned char* f, const CfgOp* op, Found* found)
{
    if (op->kind == CfgDeclare)
    {
        const VarDeclStmt* vd = (const VarDeclStmt*)op->node;
        const TypeName* declared = vd->type.name && vd->type.name[0] ? &vd->type : NULL;
        bool proves = vd->init && !vd->isRef && ConsumeValue(env, f, found, vd->init, declared, vd->base.range, true);

        KillFacts(env, f, vd->name, true);
        KillIndexVar(env, f, vd->name);

        if (proves)
        {
            SetFact(env, f, vd->name, FactNonEmpty);
        }
    }
    else if (op->kind == CfgReturn)
    {
        const ReturnStmt* rs = (const ReturnStmt*)op->node;

        if (rs->value)
        {
            ConsumeValue(env, f, found, rs->value, env->fnReturn, rs->value->range, true);
        }
    }
    else if (op->kind == CfgEval)
    {
        const Node* n = op->node;

        switch (n->kind)
        {
        case NodeAssign:
        {
            const AssignExpr* a = (const AssignExpr*)n;
            const TypeName* targetType = NodeTypesGet(env->types, a->target);
            bool proves = false;

            if (a->op == AssignSet)
            {
                proves = ConsumeValue(env, f, found, a->value, targetType, a->value->range, true);
            }

            KillPlaceOf(env, f, a->target, true);

            if (proves && targetType && targetType->isOptional)
            {
                char buf[256];
                const char* root;

                if (FactPlaceOf(a->target, buf, sizeof(buf), &root))
                {
                    SetFact(env, f, buf, FactNonEmpty);
                }
            }

            break;
        }
        case NodeIncDec:
            KillPlaceOf(env, f, ((const IncDecExpr*)n)->operand, true);
            break;
        case NodeMember:
        case NodeIndex:
            for (const Node* cur = n; cur->kind == NodeMember || cur->kind == NodeIndex;)
            {
                const Node* base = cur->kind == NodeMember ? ((const MemberExpr*)cur)->base_node
                                                           : ((const IndexExpr*)cur)->base_node;
                const TypeName* bt = ValueType(env, base);

                if (bt && bt->isOptional)
                {
                    RequireProof(env, f, found, base, cur->range);
                }

                cur = base;
            }

            break;
        case NodeCall:
            CallFactStep(env, f, found, (const CallExpr*)n);
            break;
        case NodeStructInit:
        {
            const StructInitExpr* e = (const StructInitExpr*)n;
            const TypeName* st = NodeTypesGet(env->types, n);

            for (size_t i = 0; i < e->fields.count; i++)
            {
                const StructInitField* fld = (const StructInitField*)VecGet(&e->fields, i);
                ConsumeValue(env, f, found, fld->value, FieldType(env, st, fld->name), fld->value->range, true);
            }

            break;
        }
        case NodeArrayInit:
        {
            const ArrayInitExpr* e = (const ArrayInitExpr*)n;

            for (size_t i = 0; i < e->elements.count; i++)
            {
                const Node* el = (const Node*)VecGet(&e->elements, i);
                ConsumeValue(env, f, found, el, e->elementType, el->range, e->repeatCount == NULL);
            }

            break;
        }
        default:
            break;
        }
    }
}

/* Loans. Within one expression, a place taken by an earlier operand stays loaned until the expression
   ends: shared for a borrow, mutable for a `ref`. A later operand may not move or mutate a place that
   overlaps a shared loan, nor touch one that overlaps a mutable loan. A move needs no loan of its own:
   using the place afterwards is the move checker's error. Trivial values are copied at once, so they
   hold no loan. */
typedef enum {
    LoanShared,
    LoanMut,
    LoanMove,
} LoanMode;

typedef struct {
    char key[160];
    LoanMode mode;
    const Node* node;
} Access;

typedef struct {
    Access* items;
    size_t count;
    size_t cap;
} Accesses;

static bool NonTrivial(const Env* env, const Node* n)
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

/* Could the two places share storage? One is a prefix of the other, index groups matching loosely. */
static bool PlacesOverlap(const char* a, const char* b)
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

/* How call argument `i` is taken. */
static LoanMode ArgMode(const CallExpr* e, size_t i)
{
    if (e->isIntrinsicCall)
    {
        if ((strcmp(e->callee, "drop") == 0 && i == 0) || (strcmp(e->callee, "array_push") == 0 && i == 1))
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

/* Every non-trivial place `n` touches while it is evaluated, with how. `mode` is how `n` itself is taken. */
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

        /* An index that is itself an expression runs too. */
        for (const Node* cur = n; cur->kind == NodeMember || cur->kind == NodeIndex;)
        {
            if (cur->kind == NodeIndex)
            {
                const IndexExpr* ix = (const IndexExpr*)cur;

                if (ix->index->kind != NodeIdent && ix->index->kind != NodeIntLiteral)
                {
                    CollectAccesses(env, acc, ix->index, LoanShared);
                }

                cur = ix->base_node;
            }
            else
            {
                cur = ((const MemberExpr*)cur)->base_node;
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
        CollectAccesses(env, acc, ((const MemberExpr*)n)->base_node, LoanShared);
        break;
    case NodeIndex:
        CollectAccesses(env, acc, ((const IndexExpr*)n)->base_node, LoanShared);
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

/* The operands of `n` in evaluation order, with how each is taken; false when `n` has no such structure. */
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
        ops[0] = ((const IndexExpr*)n)->base_node;
        ops[1] = ((const IndexExpr*)n)->index;
        modes[0] = modes[1] = LoanShared;
        *count = 2;
        return true;
    }

    if (n->kind == NodeAssign && ((const AssignExpr*)n)->target->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)((const AssignExpr*)n)->target;
        ops[0] = ix->base_node;
        ops[1] = ix->index;
        modes[0] = LoanMut;
        modes[1] = LoanShared;
        *count = 2;
        return true;
    }

    return false;
}

static void CheckOperandPair(const Env* env, Found* found, const Node* earlier, LoanMode earlierMode,
                             const Node* later, LoanMode laterMode)
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
                Report(found, effects.items[j].node->range, arena_strdup(env->arena, effects.items[j].key));
            }
        }
    }

    free(loans.items);
    free(effects.items);
}

static void CheckGlobalArgs(const Env* env, Found* found, const CallExpr* e);

/* Checks the loans that the operands of expression `n` take against each other. */
static void LoanCheck(const Env* env, Found* found, const Node* n)
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

        free(ws->fns[i].keys);
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
static void CheckGlobalArgs(const Env* env, Found* found, const CallExpr* e)
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
            Report(found, arg->range, arena_strdup(env->arena, buf));
        }
    }
}

/* Moving an Owning value out of an indexed element (or a field of one) is forbidden: it would leave a
   hole in the array. An optional element is fine, null being its drop flag. */
static void ElementMoveCheck(const Env* env, Found* found, const CfgOp* op)
{
    const Node* n = op->node;

    if (!op->consumed || (n->kind != NodeIndex && n->kind != NodeMember))
    {
        return;
    }

    const TypeName* t = ValueType(env, n);

    if (!t || t->isOptional || !TypeOwns(env, t))
    {
        return;
    }

    for (const Node* cur = n; cur->kind == NodeIndex || cur->kind == NodeMember;)
    {
        if (cur->kind == NodeIndex)
        {
            char buf[256];
            const char* root;
            Report(found, n->range,
                   PlaceKeyImpl(n, buf, sizeof(buf), &root, true) ? arena_strdup(env->arena, buf) : "<element>");
            return;
        }

        cur = ((const MemberExpr*)cur)->base_node;
    }
}

/* A value with a `Drop` method is moved whole or not at all: `Drop` expects every field to be there. */
static void DropFieldMoveCheck(const Env* env, Found* found, const CfgOp* op)
{
    const Node* n = op->node;

    if (!op->consumed || n->kind != NodeMember || env->inDropMethod)
    {
        return;
    }

    const TypeName* t = ValueType(env, n);

    if (!t || t->isOptional || !TypeOwns(env, t))
    {
        return;
    }

    for (const Node* cur = n; cur->kind == NodeMember; cur = ((const MemberExpr*)cur)->base_node)
    {
        const TypeName* bt = ValueType(env, ((const MemberExpr*)cur)->base_node);

        while (bt && bt->isBox && bt->inner)
        {
            bt = bt->inner;
        }

        const StructType* st = bt && bt->name && !bt->isRc && !bt->isArray
                                   ? TypeRegistryFind(env->reg, TypeRegistryResolveAlias(env->reg, bt->name))
                                   : NULL;

        if (st && !st->opaque && st->dropFunction)
        {
            char buf[256];
            const char* root;
            Report(found, n->range,
                   PlaceKeyImpl(n, buf, sizeof(buf), &root, true) ? arena_strdup(env->arena, buf) : "<field>");
            return;
        }
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
            LoanCheck(env, found, op->node);
            ElementMoveCheck(env, found, op);
            DropFieldMoveCheck(env, found, op);
        }

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

static bool IsDropMethod(const FunctionDecl* fn)
{
    size_t n = strlen(fn->name);
    return n >= 4 && strcmp(fn->name + n - 4, "Drop") == 0;
}

size_t MoveCheckFunction(const FunctionDecl* fn, const TypeRegistry* reg, const NodeTypes* types,
                         const WriteSets* ws, Arena* arena, MoveDiag** out)
{
    *out = NULL;

    if (!fn || !fn->body)
    {
        return 0;
    }

    Env env = {.reg = reg, .types = types, .arena = arena, .fnReturn = &fn->returnType, .ws = ws,
               .inDropMethod = IsDropMethod(fn)};
    Cfg g = CfgBuild(fn);
    CollectLocals(&env, fn, &g);
    BuildAliases(&env, &g);
    CollectPaths(&env, &g);
    CollectFacts(&env, &g);

    size_t pathCount = env.pathCount;
    size_t n = pathCount + env.factCount;
    Found found = {0};

    if (g.count > 0)
    {
        /* Entry state of every block; everything is Live on entry. */
        unsigned char* in = (unsigned char*)calloc(g.count * n + 1, 1);
        unsigned char* cur = (unsigned char*)malloc(n + 1);
        unsigned char* edge = (unsigned char*)malloc(n + 1);

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
                    const CfgOp* last = blk->opCount ? &blk->ops[blk->opCount - 1] : NULL;

                    memcpy(edge, cur, n);

                    if (last && last->kind == CfgCondition)
                    {
                        ApplyCond(&env, edge + pathCount, last->node, s == 0);
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
        free(edge);
    }

    for (size_t i = 0; i < env.pathCount; i++)
    {
        free(env.paths[i]);
    }

    free(env.paths);
    for (size_t i = 0; i < env.factCount; i++)
    {
        free(env.facts[i]);
    }

    free(env.facts);
    FreeAliases(&env);
    free(env.locals.locals);
    free(env.named.locals);
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

    if (nameLen == 0 || nameLen >= cap
        || (strcmp(close + 1, " used after move") != 0 && strncmp(close + 1, " is poisoned", 12) != 0
            && strncmp(close + 1, " has not been blessed", 21) != 0
            && strncmp(close + 1, " is definitely empty", 20) != 0))
    {
        return false;
    }

    for (size_t i = 0; i < nameLen; i++)
    {
        char c = message[1 + i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '[' || c == ']' || c == '<' || c == '>'))
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
    WriteSets* ws = WriteSetsBuild(mod, reg, types, arena);

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* fn = (const FunctionDecl*)VecGet(&mod->functions, i);
        MoveDiag* mine = NULL;
        size_t mineCount = MoveCheckFunction(fn, reg, types, ws, arena, &mine);

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

    if (getenv("STRATA_SHADOW_VERBOSE"))
    {
        for (size_t m = 0; m < all.count; m++)
        {
            fprintf(stderr, "shadow: new %u:%u '%s'\n", all.items[m].range.fileId,
                    all.items[m].range.start, all.items[m].name);
        }
    }

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
    WriteSetsFree(ws);
    free(all.items);
    return disagreements;
}
