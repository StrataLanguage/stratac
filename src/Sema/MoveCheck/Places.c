#include "Sema/MoveCheck/MoveCheckInternal.h"

#include <inttypes.h>

/* The aliases of the function being checked, so that place keys can name the bound place. */
static const Env* aliasEnv;

int FindLocal(const Locals* ls, const char* name)
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

bool TypeOwns(const Env* env, const TypeName* t)
{
    return t && t->name && TypeClassify(env->reg, env->arena, t).owning;
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

const TypeName* DeclType(const Env* env, const VarDeclStmt* vd)
{
    if (vd->type.name && vd->type.name[0])
    {
        return &vd->type;
    }

    return NodeTypesGet(env->types, vd->init);
}

/* The path of a variable-and-fields place into `buf`, and the variable's name; false for anything else. */
bool PlaceOf(const Node* n, char* buf, size_t cap, const char** root)
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

    if (m->isImplProperty || m->isEnumConst || m->isScalarConst || !PlaceOf(m->baseNode, buf, cap, root))
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
bool TrackedPlace(const Env* env, const Node* n, char* buf, size_t cap)
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
bool IsAncestor(const char* a, const char* d)
{
    size_t n = strlen(a);
    return strncmp(a, d, n) == 0 && d[n] == '.';
}

/* The declared type of the field `name` of the struct `t` (looking through `^`, `?` and aliases). */
const TypeName* FieldType(const Env* env, const TypeName* t, const char* name)
{
    while (t && (TypeNameIsBox(t) || TypeNameIsOptional(t)) && t->inner)
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
const TypeName* PlaceType(const Env* env, const Node* n)
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
        return FieldType(env, PlaceType(env, m->baseNode), m->member);
    }

    return NULL;
}

void CollectLocals(Env* env, const FunctionDecl* fn, const Cfg* g)
{
    for (size_t i = 0; i < fn->params.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet(&fn->params, i);

        AddLocalTo(&env->named, p->name, false, NULL);

        if (p->mod == ModNone && !p->inferredBorrow && !fn->isExtern)
        {
            AddLocal(env, p->name, TypeOwns(env, &p->type), &p->type);
        }
        else if (p->mod == ModRef && !p->type.isConst && !fn->isExtern)
        {
            /* Parts of a ref parameter may be taken, but only if they are put back before returning. */
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
void CollectPaths(Env* env, const Cfg* g)
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

unsigned char Join(unsigned char a, unsigned char b)
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
int PlaceBlocker(const Env* env, const unsigned char* state, const char* path, bool wholeUse)
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
            return (int)i;
        }
    }

    return -1;
}

/* Marks a place Moved, or makes it and every part of it Live again. */
void SetPlace(const Env* env, unsigned char* state, const char* path, unsigned char value)
{
    for (size_t i = 0; i < env->pathCount; i++)
    {
        if (strcmp(env->paths[i], path) == 0 || (value == StateLive && IsAncestor(path, env->paths[i])))
        {
            state[i] = value;
        }
    }
}

bool IsCopied(const Env* env, const Node* n)
{
    for (size_t i = 0; i < env->copiedCount; i++)
    {
        if (env->copied[i] == n)
        {
            return true;
        }
    }

    return false;
}

static void NoteIfCopied(Env* env, const Node* value, const TypeName* target)
{
    const TypeName* vt = value ? ValueType(env, value) : NULL;

    if (!vt || !target || !target->name || !TypeNameIsBare(target))
    {
        return;
    }

    /* A box handed to a bare `T` copies the pointee when it is plain data. A pointee that owns something cannot be
       copied (it would be dropped twice), so the box is emptied into the new place instead. A bare struct element
       is a deep copy. */
    if (TypeNameIsBox(vt))
    {
        if (!vt->inner || TypeClassify(env->reg, env->arena, vt->inner).owning)
        {
            return;
        }
    }

    if (!TypeNameIsBox(vt))
    {
        if (value->kind != NodeIndex)
        {
            return;
        }

        const StructType* st = vt->name && TypeNameIsBare(vt)
                                   ? TypeRegistryFind(env->reg, TypeRegistryResolveAlias(env->reg, vt->name))
                                   : NULL;

        if (!st || st->opaque)
        {
            return;
        }
    }

    if (env->copiedCount == env->copiedCap)
    {
        env->copiedCap = env->copiedCap ? env->copiedCap * 2 : 8;
        env->copied = (const Node**)realloc((void*)env->copied, env->copiedCap * sizeof(*env->copied));
    }

    env->copied[env->copiedCount++] = value;
}

void CollectCopies(Env* env, const Cfg* g)
{
    for (size_t b = 0; b < g->count; b++)
    {
        for (size_t j = 0; j < g->blocks[b].opCount; j++)
        {
            const CfgOp* op = &g->blocks[b].ops[j];

            if (op->kind == CfgDeclare)
            {
                const VarDeclStmt* vd = (const VarDeclStmt*)op->node;
                NoteIfCopied(env, vd->isRef ? NULL : vd->init, vd->type.name && vd->type.name[0] ? &vd->type : NULL);
            }
            else if (op->kind == CfgReturn)
            {
                NoteIfCopied(env, ((const ReturnStmt*)op->node)->value, env->fnReturn);
            }
            else if (op->kind == CfgEval && op->node->kind == NodeAssign)
            {
                const AssignExpr* a = (const AssignExpr*)op->node;
                NoteIfCopied(env, a->op == AssignSet ? a->value : NULL, NodeTypesGet(env->types, a->target));
            }
            else if (op->kind == CfgEval && op->node->kind == NodeCall && !((const CallExpr*)op->node)->isIntrinsicCall)
            {
                const CallExpr* e = (const CallExpr*)op->node;
                const FunctionDecl* d = e->resolvedDecl;

                for (size_t i = 0; d && d->params.count > 0 && i < e->args.count; i++)
                {
                    const ParamDecl* p
                        = (const ParamDecl*)VecGet(&d->params, i < d->params.count ? i : d->params.count - 1);

                    if (i >= d->params.count && !p->isVarargRest)
                    {
                        break;
                    }

                    /* A typed rest parameter takes each argument as one element. */
                    NoteIfCopied(env, (const Node*)VecGet(&e->args, i),
                                 p->isVarargRest ? (TypeNameHasArrayShape(&p->type) ? p->type.elem : NULL) : &p->type);
                }
            }
            else if (op->kind == CfgEval && op->node->kind == NodeStructInit)
            {
                const StructInitExpr* e = (const StructInitExpr*)op->node;

                for (size_t i = 0; i < e->fields.count; i++)
                {
                    const StructInitField* fld = (const StructInitField*)VecGet(&e->fields, i);
                    NoteIfCopied(env, fld->value,
                                 fld->name ? FieldType(env, NodeTypesGet(env->types, op->node), fld->name) : NULL);
                }
            }
        }
    }
}

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

void FreeAliases(Env* env)
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
    aliasEnv = NULL;
}

void BuildAliases(Env* env, const Cfg* g)
{
    char buf[256];
    const char* root;
    aliasEnv = env;

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

const FnWrites* FindWrites(const WriteSets* ws, const FunctionDecl* d)
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

/* The place of a variable-and-fields-and-indices expression (`a`, `a.b`, `a[i].b`, `a[0]`). An index
   is a variable or an integer literal; any other index is no place. */
const char* AliasTarget(const char* name)
{
    const Env* env = aliasEnv;

    for (size_t i = 0; env && i < env->aliasCount; i++)
    {
        if (strcmp(env->aliasName[i], name) == 0)
        {
            return env->aliasTarget[i][0] ? env->aliasTarget[i] : NULL;
        }
    }

    return NULL;
}

/* A canonical spelling of an index that is built from variables, literals, fields, other elements and
   arithmetic, so that the same expression written twice names the same element. */
bool IndexSpelling(const Node* n, char* out, size_t cap)
{
    const char* root;

    switch (n->kind)
    {
    case NodeIdent:
    case NodeMember:
    case NodeIndex:
        return PlaceKeyImpl(n, out, cap, &root, false);
    case NodeIntLiteral:
        snprintf(out, cap, "%" PRIu64, ((const IntLiteral*)n)->value);
        return true;
    case NodeCast:
        return IndexSpelling(((const CastExpr*)n)->operand, out, cap);
    case NodeBinary:
    {
        const BinaryExpr* b = (const BinaryExpr*)n;
        char l[64];
        char r[64];

        if (b->op == BinLogicAnd || b->op == BinLogicOr || !IndexSpelling(b->lhs, l, sizeof(l))
            || !IndexSpelling(b->rhs, r, sizeof(r)))
        {
            return false;
        }

        snprintf(out, cap, "(%s#%d#%s)", l, (int)b->op, r);
        return true;
    }
    default:
        return false;
    }
}
