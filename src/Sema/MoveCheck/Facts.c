// Narrowing and ref facts: which optional places are proven non-empty, and what a write invalidates.
#include "Sema/MoveCheck/MoveCheckInternal.h"

// What a value is handed to when only "not an optional" matters: a C vararg, a `substring` operand.
static const TypeName PLAIN_TARGET = {.name = "?"};

static bool FactPlaceOf(const Node* n, char* buf, size_t cap, const char** root)
{
    return PlaceKeyImpl(n, buf, cap, root, false);
}

// With `lax`, an index that is neither a variable nor a literal is `[?]`, which may be any element.
bool PlaceKeyImpl(const Node* n, char* buf, size_t cap, const char** root, bool lax)
{
    if (!n)
    {
        return false;
    }

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
    char suffix[160];

    if (n->kind == NodeMember)
    {
        const MemberExpr* m = (const MemberExpr*)n;

        if (m->isImplProperty || m->isEnumConst || m->isScalarConst || strlen(m->member) + 2 > sizeof(suffix))
        {
            return false;
        }

        base = m->baseNode;
        snprintf(suffix, sizeof(suffix), ".%s", m->member);
    }
    else if (n->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)n;
        base = ix->baseNode;
        char spelling[140];

        if (IndexSpelling(ix->index, spelling, sizeof(spelling)))
        {
            snprintf(suffix, sizeof(suffix), "[%s]", spelling);
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

size_t SplitPlace(const char* path, Seg* segs)
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
            for (int depth = 0; *s; s++)
            {
                depth += *s == '[';
                depth -= *s == ']';

                if (depth == 0)
                {
                    s++;
                    break;
                }
            }
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

// Can the two segments name the same thing? Different literal indices cannot; a variable index can be anything.
bool SegsAlias(Seg a, Seg b)
{
    if (a.p[0] == '[' && b.p[0] == '[' && !(a.p[1] >= '0' && a.p[1] <= '9' && b.p[1] >= '0' && b.p[1] <= '9'))
    {
        return true;
    }

    return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

// The type of a value: what sema inferred, else a call's declared return type.
const TypeName* ValueType(const Env* env, const Node* n)
{
    const TypeName* t = NodeTypesGet(env->types, n);

    if (!t && n->kind == NodeCall && ((const CallExpr*)n)->resolvedDecl)
    {
        t = &((const CallExpr*)n)->resolvedDecl->returnType;
    }

    return t;
}

// Registers every optional place on the chain `n`, `n`'s base, and so on.
static void AddOptionalChain(Env* env, const Node* n)
{
    char buf[256];
    const char* root;

    while (n)
    {
        const TypeName* t = NodeTypesGet(env->types, n);

        if (t && TypeNameIsOptional(t) && FactPlaceOf(n, buf, sizeof(buf), &root))
        {
            AddFact(env, buf);
        }

        if (n->kind == NodeMember)
        {
            n = ((const MemberExpr*)n)->baseNode;
        }
        else if (n->kind == NodeIndex)
        {
            n = ((const IndexExpr*)n)->baseNode;
        }
        else
        {
            break;
        }
    }
}

void CollectFacts(Env* env, const Cfg* g)
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

                if (t && TypeNameIsOptional(t))
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

// The ref-state bytes of a fact array: they follow the facts in it.
static unsigned char* RefStates(const Env* env, unsigned char* f)
{
    return f + env->factCount;
}

// A write to `key` (or, with `selfToo`, to `key` itself) kills the refs bound there or under it, and the
// refs bound to any place that may be the same.
static void KillRefs(const Env* env, unsigned char* f, const char* key, bool selfToo)
{
    Seg a[MaxSegs];
    size_t na = SplitPlace(key, a);

    for (size_t i = 0; i < env->aliasCount; i++)
    {
        Seg b[MaxSegs];
        size_t nb = env->aliasTarget[i][0] ? SplitPlace(env->aliasTarget[i], b) : 0;
        bool hit = nb > na || (nb == na && nb > 0 && selfToo);

        for (size_t k = 0; hit && k < na; k++)
        {
            hit = SegsAlias(a[k], b[k]);
        }

        if (hit)
        {
            RefStates(env, f)[i] = StateMoved;
        }
    }
}

static void KillFacts(const Env* env, unsigned char* f, const char* key, bool selfToo)
{
    Seg a[MaxSegs];
    size_t na = SplitPlace(key, a);

    KillRefs(env, f, key, selfToo);

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

// The variable `name` changed: facts about elements indexed by it no longer hold.
static void KillIndexVar(const Env* env, unsigned char* f, const char* name)
{
    size_t len = strlen(name);

    for (size_t i = 0; i < env->factCount; i++)
    {
        Seg b[MaxSegs];
        size_t nb = SplitPlace(env->facts[i], b);

        for (size_t s = 0; s < nb; s++)
        {
            for (size_t k = 0; b[s].p[0] == '[' && k + len < b[s].n; k++)
            {
                bool startsWord = k == 0 || !(isalnum((unsigned char)b[s].p[k - 1]) || b[s].p[k - 1] == '_');
                bool endsWord = !(isalnum((unsigned char)b[s].p[k + len]) || b[s].p[k + len] == '_');

                if (startsWord && endsWord && memcmp(b[s].p + k, name, len) == 0)
                {
                    f[i] = FactNone;
                }
            }
        }
    }
}

// A call may write any global.
static void KillGlobals(const Env* env, unsigned char* f)
{
    for (size_t i = 0; i < env->aliasCount; i++)
    {
        Seg b[MaxSegs];
        char root[128];

        if (env->aliasTarget[i][0] && SplitPlace(env->aliasTarget[i], b) > 0 && b[0].n < sizeof(root))
        {
            memcpy(root, b[0].p, b[0].n);
            root[b[0].n] = 0;

            if (FindLocal(&env->named, root) < 0)
            {
                RefStates(env, f)[i] = StateMoved;
            }
        }
    }

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

// Applies what `c` evaluating to `whenTrue` proves: `x?` terms joined by `&&` (all hold when true) or
// `||` (all hold when false), under any number of `!`.
void ApplyCond(const Env* env, unsigned char* f, const Node* c, bool whenTrue, bool loopExit)
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

        if (FactPlaceOf(((const NullTestExpr*)c)->operand, buf, sizeof(buf), &root))
        {
            // A loop that ends because its test failed says nothing worth a "definitely empty".
            SetFact(env, f, buf, whenTrue ? FactNonEmpty : loopExit ? FactNone : FactEmpty);
        }
    }
    else if (c->kind == NodeBinary)
    {
        const BinaryExpr* b = (const BinaryExpr*)c;

        if ((b->op == BinLogicAnd && whenTrue) || (b->op == BinLogicOr && !whenTrue))
        {
            ApplyCond(env, f, b->lhs, whenTrue, loopExit);
            ApplyCond(env, f, b->rhs, whenTrue, loopExit);
        }
    }
}

// Reports a read of the optional `value` that lacks the proof. `k` is its fact, if it has one.
static void ReportUnproven(const Env* env, const unsigned char* f, Found* found, const Node* value, SourceRange range,
                           int k)
{
    char buf[256];
    const char* root;
    bool strict = FactPlaceOf(value, buf, sizeof(buf), &root);
    const TypeName* vt = ValueType(env, value);
    const char* name = "<expression>";

    if (strict)
    {
        name = ArenaStrdup(env->arena, buf);
    }
    else if (PlaceKeyImpl(value, buf, sizeof(buf), &root, true))
    {
        // An index that is neither a variable nor a literal: show it erased, as `a[]`.
        char* q = strstr(buf, "[?]");

        while (q)
        {
            memmove(q + 1, q + 2, strlen(q + 2) + 1);
            q = strstr(q, "[?]");
        }

        name = ArenaStrdup(env->arena, buf);
    }

    MoveDiag* d = ReportAs(found, MoveDiagNarrow, range, name);

    if (d)
    {
        d->type = vt ? vt->name : "";

        if (!strict && strchr(name, '['))
        {
            d->detail = "untrackable index";
        }
        else if (k >= 0 && f[k] == FactEmpty)
        {
            d->detail = "definitely empty";
        }
    }
}

// Reading through a `T?` needs the proof.
static void RequireProof(const Env* env, const unsigned char* f, Found* found, const Node* value, SourceRange range)
{
    char buf[256];
    const char* root;
    int k = FactPlaceOf(value, buf, sizeof(buf), &root) ? FindFact(env, buf) : -1;

    if (found && !(k >= 0 && f[k] == FactNonEmpty))
    {
        ReportUnproven(env, f, found, value, range, k);
    }
}

// `value` is handed to something of type `target` (NULL: unknown, so no proof is wanted). Returns whether
// the target may count it as non-empty. A moved optional leaves its source empty.
static bool ConsumeValue(const Env* env, unsigned char* f, Found* found, const Node* value, const TypeName* target,
                         SourceRange range, bool move)
{
    if (!value)
    {
        return false;
    }

    const TypeName* vt = ValueType(env, value);

    if (!vt || !TypeNameIsOptional(vt))
    {
        // Moving an owning value out ends every `ref` into it.
        char moved[256];
        const char* movedRoot;

        if (move && vt && !IsCopied(env, value) && TypeOwns(env, vt)
            && PlaceKeyImpl(value, moved, sizeof(moved), &movedRoot, true))
        {
            KillFacts(env, f, moved, true);
        }

        return true;
    }

    char buf[256];
    const char* root;
    bool haveKey = FactPlaceOf(value, buf, sizeof(buf), &root);
    int k = haveKey ? FindFact(env, buf) : -1;
    bool proven = k >= 0 && f[k] == FactNonEmpty;

    if (target && !TypeNameIsOptional(target) && !proven && found)
    {
        ReportUnproven(env, f, found, value, range, k);
    }

    if (move && TypeOwns(env, vt))
    {
        // A move through an index we cannot follow may empty any element it could be.
        if (haveKey || PlaceKeyImpl(value, buf, sizeof(buf), &root, true))
        {
            KillFacts(env, f, buf, true);
        }

        if (haveKey)
        {
            SetFact(env, f, buf, FactEmpty);
        }
    }

    return proven;
}

// A value being dropped runs its `drop` method (and those of its fields and elements), which may write
// globals: whatever the narrowing and refs rested on in those globals is gone.
static void ApplyDropWrites(const Env* env, unsigned char* f, const TypeName* t, int depth)
{
    while (t && (TypeNameIsBox(t) || TypeNameIsOptional(t) || TypeNameHasArrayShape(t)))
    {
        const TypeName* next = TypeNameHasArrayShape(t) ? t->elem : t->inner;

        if (!next)
        {
            return;
        }

        t = next;
    }

    if (!t || !t->name || depth > 6)
    {
        return;
    }

    const StructType* st = TypeRegistryFind(env->reg, TypeRegistryResolveAlias(env->reg, t->name));

    if (!st || st->opaque)
    {
        return;
    }

    if (st->dropFunction)
    {
        const FnWrites* w = FindWrites(env->ws, st->dropFunction);

        if (!w || w->all)
        {
            KillGlobals(env, f);
        }
        else
        {
            for (size_t i = 0; i < w->count; i++)
            {
                KillFacts(env, f, w->keys[i], true);
            }
        }
    }

    for (size_t i = 0; i < st->fields.count; i++)
    {
        ApplyDropWrites(env, f, &((const FieldDecl*)VecGet((Vec*)&st->fields, i))->type, depth + 1);
    }
}

// `key` was written or moved out of: forget what was proven about it, about what is under it, and about
// every place that may be the same (`a[j]` for `a[i]`). With `selfToo` false only what is under it.
// A `ref` local stays valid until what it was bound to changes. Its state follows the facts in the array.
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
        else
        {
            // Writing `other[0]` may change what `foo[other[bar]]` names.
            Seg segs[MaxSegs];

            if (SplitPlace(buf, segs) > 0 && segs[0].n < sizeof(buf))
            {
                char name[128];

                if (segs[0].n < sizeof(name))
                {
                    memcpy(name, segs[0].p, segs[0].n);
                    name[segs[0].n] = 0;
                    KillIndexVar(env, f, name);
                }
            }
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
            ApplyDropWrites(env, f, ValueType(env, arg0), 0);
        }
        else if (arg1 && strcmp(e->callee, "array_push") == 0)
        {
            const TypeName* at = arg0 ? ValueType(env, arg0) : NULL;
            ConsumeValue(env, f, found, arg1, at && TypeNameHasArrayShape(at) ? at->elem : NULL, arg1->range, true);
        }
        else if (arg0 && e->args.count > 2 && strcmp(e->callee, "array_replace") == 0)
        {
            const Node* val = (const Node*)VecGet(&e->args, 2);
            const TypeName* at = ValueType(env, arg0);
            ConsumeValue(env, f, found, val, at && TypeNameHasArrayShape(at) ? at->elem : NULL, val->range, true);
        }
        else if (arg0 && strcmp(e->callee, "substring") == 0)
        {
            ConsumeValue(env, f, found, arg0, &PLAIN_TARGET, arg0->range, false);
        }

        if (arg0 && IsArrayMutator(e->callee))
        {
            // Pushing or reserving moves no element: only a place indexed by the array's length goes. The
            // others change what the elements are. The array itself stays set either way.
            bool keepsElements = (ArrayBuiltinFlagsOf(e->callee) & ABF_KEEPS_ELEMENTS) != 0;
            const char* name = arg0->kind == NodeIdent    ? ((const IdentExpr*)arg0)->name
                               : arg0->kind == NodeMember ? ((const MemberExpr*)arg0)->member
                                                          : NULL;

            if (keepsElements && name)
            {
                KillIndexVar(env, f, name);
            }

            // Growing can move the buffer, so refs into the elements go either way.
            if (keepsElements)
            {
                char key[256];
                const char* keyRoot;

                if (PlaceKeyImpl(arg0, key, sizeof(key), &keyRoot, true))
                {
                    KillRefs(env, f, key, false);
                }
            }

            if (!keepsElements)
            {
                KillPlaceOf(env, f, arg0, false);
            }
        }

        return;
    }

    const FunctionDecl* d = e->resolvedDecl;

    for (size_t i = 0; d && i < e->args.count; i++)
    {
        const Node* arg = (const Node*)VecGet(&e->args, i);
        const ParamDecl* last = d->params.count ? (const ParamDecl*)VecGet(&d->params, d->params.count - 1) : NULL;
        const ParamDecl* p = i < d->params.count          ? (const ParamDecl*)VecGet(&d->params, i)
                             : last && last->isVarargRest ? last
                                                          : NULL;
        bool byValue = p && !d->isExtern && p->mod == ModNone && !p->inferredBorrow;

        ConsumeValue(env, f, found, arg, p && !p->isVarargRest ? &p->type : &PLAIN_TARGET, arg->range, byValue);

        if (p && p->mod == ModRef && !p->type.isConst)
        {
            // Only a `ref T?` can empty the optional itself; a `ref T` changes what is inside.
            KillPlaceOf(env, f, arg, TypeNameIsOptional(&p->type));
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

// The name at the root of `n`: the variable a place expression starts from.
const IdentExpr* RootIdent(const Node* n)
{
    while (n && (n->kind == NodeMember || n->kind == NodeIndex))
    {
        n = n->kind == NodeMember ? ((const MemberExpr*)n)->baseNode : ((const IndexExpr*)n)->baseNode;
    }

    return n && n->kind == NodeIdent ? (const IdentExpr*)n : NULL;
}

// Using a `ref` after what it was bound to changed.
void DanglingCheck(const Env* env, const unsigned char* f, Found* found, const Node* n)
{
    const IdentExpr* id = NULL;

    if (n->kind == NodeIdent || n->kind == NodeMember || n->kind == NodeIndex)
    {
        id = RootIdent(n);
    }
    else if (n->kind == NodeAssign)
    {
        id = RootIdent(((const AssignExpr*)n)->target);
    }

    for (size_t i = 0; id && i < env->aliasCount; i++)
    {
        if (strcmp(env->aliasName[i], id->name) == 0 && env->aliasTarget[i][0] && f[env->factCount + i] != StateLive)
        {
            ReportAs(found, MoveDiagDangle, id->base.range, id->name);
        }
    }
}

static void FactDeclare(const Env* env, unsigned char* f, const CfgOp* op, Found* found)
{
    const VarDeclStmt* vd = (const VarDeclStmt*)op->node;
    const TypeName* declared = vd->type.name && vd->type.name[0] ? &vd->type : NULL;
    bool proves = vd->init && !vd->isRef && ConsumeValue(env, f, found, vd->init, declared, vd->base.range, true);

    KillFacts(env, f, vd->name, true);
    KillIndexVar(env, f, vd->name);

    for (size_t i = 0; i < env->aliasCount; i++)
    {
        if (strcmp(env->aliasName[i], vd->name) == 0)
        {
            RefStates(env, f)[i] = StateLive;
        }
    }

    if (proves)
    {
        SetFact(env, f, vd->name, FactNonEmpty);
    }
}

static void FactScopeExit(const Env* env, unsigned char* f, const CfgOp* op)
{
    // The locals of the block drop here, running their `drop` methods.
    const Block* blk = (const Block*)op->node;

    for (size_t i = 0; blk && blk->base.kind == NodeBlock && i < blk->statements.count; i++)
    {
        const Node* st = (const Node*)VecGet(&blk->statements, i);

        if (st->kind == NodeVarDecl && !((const VarDeclStmt*)st)->isRef)
        {
            const TypeName* t = DeclType(env, (const VarDeclStmt*)st);

            if (t && TypeOwns(env, t))
            {
                ApplyDropWrites(env, f, t, 0);
            }
        }
    }
}

static void FactReturn(const Env* env, unsigned char* f, const CfgOp* op, Found* found)
{
    const ReturnStmt* rs = (const ReturnStmt*)op->node;

    if (rs->value)
    {
        ConsumeValue(env, f, found, rs->value, env->fnReturn, rs->value->range, true);
    }
}

static void FactAssign(const Env* env, unsigned char* f, Found* found, const Node* n)
{
    const AssignExpr* a = (const AssignExpr*)n;
    const TypeName* targetType = NodeTypesGet(env->types, a->target);
    bool proves = false;

    // Writing through a `T?` base needs the proof, like reading through it.
    for (const Node* cur = a->target; cur && (cur->kind == NodeMember || cur->kind == NodeIndex);)
    {
        const Node* base
            = cur->kind == NodeMember ? ((const MemberExpr*)cur)->baseNode : ((const IndexExpr*)cur)->baseNode;
        const TypeName* bt = base ? ValueType(env, base) : NULL;

        if (bt && TypeNameIsOptional(bt))
        {
            RequireProof(env, f, found, base, cur->range);
        }

        cur = base;
    }

    if (a->op == AssignSet)
    {
        proves = ConsumeValue(env, f, found, a->value, targetType, a->value->range, true);
    }

    // Overwriting an owning value drops the old one.
    if (a->op == AssignSet && targetType && TypeOwns(env, targetType))
    {
        ApplyDropWrites(env, f, targetType, 0);
    }

    KillPlaceOf(env, f, a->target, true);

    if (proves && targetType && TypeNameIsOptional(targetType))
    {
        char buf[256];
        const char* root;

        if (FactPlaceOf(a->target, buf, sizeof(buf), &root))
        {
            SetFact(env, f, buf, FactNonEmpty);
        }
    }
}

static void FactBinary(const Env* env, unsigned char* f, Found* found, const Node* n)
{
    const BinaryExpr* b = (const BinaryExpr*)n;
    const TypeName* lt = ValueType(env, b->lhs);
    const TypeName* rt = ValueType(env, b->rhs);
    bool lo = lt && TypeNameIsOptional(lt);
    bool ro = rt && TypeNameIsOptional(rt);
    bool relational = b->op == BinLt || b->op == BinLtEq || b->op == BinGt || b->op == BinGtEq;

    if (b->op != BinLogicAnd && b->op != BinLogicOr)
    {
        // `a == b` of two optionals compares them whole; anything else reads the value.
        if (lo && (relational || !ro))
        {
            RequireProof(env, f, found, b->lhs, b->lhs->range);
        }

        if (ro && (relational || !lo))
        {
            RequireProof(env, f, found, b->rhs, b->rhs->range);
        }
    }
}

static void FactMemberIndex(const Env* env, unsigned char* f, Found* found, const Node* n)
{
    for (const Node* cur = n; cur && (cur->kind == NodeMember || cur->kind == NodeIndex);)
    {
        const Node* base
            = cur->kind == NodeMember ? ((const MemberExpr*)cur)->baseNode : ((const IndexExpr*)cur)->baseNode;
        const TypeName* bt = base ? ValueType(env, base) : NULL;

        if (bt && TypeNameIsOptional(bt))
        {
            RequireProof(env, f, found, base, cur->range);
        }

        cur = base;
    }
}

static void FactStructInit(const Env* env, unsigned char* f, Found* found, const Node* n)
{
    const StructInitExpr* e = (const StructInitExpr*)n;
    const TypeName* st = NodeTypesGet(env->types, n);

    for (size_t i = 0; i < e->fields.count; i++)
    {
        const StructInitField* fld = (const StructInitField*)VecGet(&e->fields, i);

        if (!fld->value)
        {
            continue;
        }

        ConsumeValue(env, f, found, fld->value, fld->name ? FieldType(env, st, fld->name) : NULL, fld->value->range,
                     true);
    }
}

static void FactArrayInit(const Env* env, unsigned char* f, Found* found, const Node* n)
{
    const ArrayInitExpr* e = (const ArrayInitExpr*)n;

    for (size_t i = 0; i < e->elements.count; i++)
    {
        const Node* el = (const Node*)VecGet(&e->elements, i);

        if (el)
        {
            ConsumeValue(env, f, found, el, e->elementType, el->range, e->repeatCount == NULL);
        }
    }
}

static void FactEval(const Env* env, unsigned char* f, const CfgOp* op, Found* found)
{
    const Node* n = op->node;

    switch (n->kind)
    {
    case NodeAssign:
        FactAssign(env, f, found, n);
        break;
    case NodeIncDec:
        KillPlaceOf(env, f, ((const IncDecExpr*)n)->operand, true);
        break;
    case NodeBinary:
        FactBinary(env, f, found, n);
        break;
    case NodeMember:
    case NodeIndex:
        FactMemberIndex(env, f, found, n);
        break;
    case NodeCall:
        CallFactStep(env, f, found, (const CallExpr*)n);
        break;
    case NodeStructInit:
        FactStructInit(env, f, found, n);
        break;
    case NodeArrayInit:
        FactArrayInit(env, f, found, n);
        break;
    default:
        break;
    }
}

// The effect of one op on the narrowing facts; with `found` set, reads that lack the proof are reported.
void FactStep(const Env* env, unsigned char* f, const CfgOp* op, Found* found)
{
    if (op->kind == CfgDeclare)
    {
        FactDeclare(env, f, op, found);
    }
    else if (op->kind == CfgScopeExit)
    {
        FactScopeExit(env, f, op);
    }
    else if (op->kind == CfgReturn)
    {
        FactReturn(env, f, op, found);
    }
    else if (op->kind == CfgEval)
    {
        FactEval(env, f, op, found);
    }
}
