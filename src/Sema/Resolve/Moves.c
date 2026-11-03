/* Move-check bookkeeping: move keys, moved and emptied optional sources, and flow state joins. */
#include "Sema/Resolve/ResolverInternal.h"

static const TypeName* KeyPathType(Resolver* r, const char* key);

bool IsBoxGlobalName(const Resolver* r, const char* name)
{
    return StrMapGet(&r->boxGlobals, name) != NULL;
}

// Root binding of a move key ("holder.gun" -> "holder").
static const char* KeyRoot(Arena* arena, const char* key)
{
    const char* dot = strchr(key, '.');
    const char* base = dot ? ArenaStrndup(arena, key, (size_t)(dot - key)) : key;

    /* Drop the trailing bracket group (erased/var/literal). */
    const char* open = strrchr(base, '[');

    if (open)
    {
        return ArenaStrndup(arena, base, (size_t)(open - base));
    }

    return base;
}

// Mark `name` moved, unless it is a borrowed global or ref param.
/* The type of a move path ("a.b[i].c" as MovableBoxSourceKey spells it). NULL when it cannot be typed. */
static const TypeName* KeyPathType(Resolver* r, const char* key)
{
    if (!r->curScope || !key)
    {
        return NULL;
    }

    size_t at = 0;

    while (key[at] && key[at] != '.' && key[at] != '[')
    {
        at++;
    }

    const TypeName* t = (const TypeName*)StrMapGet(r->curScope, ArenaStrndup(r->arena, key, at));

    while (t && key[at])
    {
        if ((TypeNameIsBox(t) || TypeNameIsOptional(t)) && t->inner)
        {
            t = t->inner;
        }

        if (key[at] == '.')
        {
            size_t start = ++at;

            while (key[at] && key[at] != '.' && key[at] != '[')
            {
                at++;
            }

            const char* field = ArenaStrndup(r->arena, key + start, at - start);
            const StructType* st = TypeRegistryFind(&r->registry, TypeRegistryResolveAlias(&r->registry, t->name));
            int idx = st ? TypeRegistryFieldIndex(&r->registry, st->name, field) : -1;
            t = idx >= 0 ? &((const FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx))->type : NULL;
        }
        else
        {
            int depth = 0;

            do
            {
                depth += key[at] == '[' ? 1 : key[at] == ']' ? -1 : 0;
                at++;
            } while (key[at] && depth > 0);

            t = TypeNameArrayElem(t);
        }
    }

    return t;
}

/* `key` with a `ref` local at its root replaced by the path the local aliases. */
const char* CanonicalRefKey(Resolver* r, const char* key)
{
    const char* root = KeyRoot(r->arena, key);
    const char* aliased = (const char*)StrMapGet(&r->refLocals, root);

    return aliased ? ArenaFormat(r->arena, "%s%s", aliased, key + strlen(root)) : key;
}

/* True when the root of move key `key` is a module global (not a local of the same name): no function
   owns a global, so nothing may be moved out of any path into it — a field of a global struct included. */
static bool IsGlobalRootKey(Resolver* r, const char* key)
{
    const char* root = KeyRoot(r->arena, key);

    for (size_t i = 0; i < r->mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->mod->globals, i);

        if (strcmp(gd->name, root) == 0)
        {
            return !r->curScope || StrMapGet(r->curScope, root) == (void*)&gd->type;
        }
    }

    return false;
}

static void DiagMoveFromGlobal(Resolver* r, SourceRange range, const char* name)
{
    DiagErrorFmt(r->diag, range,
                 "'%s' cannot be moved as it is not owned because it is global. (use a `[const] ref ^T`, copy() "
                 "it, or pass `T` by value)",
                 name);
}

/* True when the root of move key `key` is a `const ref` param or local. It only borrows what it names
   and nothing can be put back through it, so nothing may be moved out of any path into it. */
static bool IsConstRefRootKey(Resolver* r, const char* key)
{
    const char* root = KeyRoot(r->arena, key);
    const TypeName* t = r->curScope ? (const TypeName*)StrMapGet(r->curScope, root) : NULL;

    if (!t || !t->isConst)
    {
        return false;
    }

    if (StrMapGet(&r->refLocals, root))
    {
        return true;
    }

    for (size_t i = 0; r->currentFunction && i < r->currentFunction->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&r->currentFunction->params, i);

        if (&p->type == t && p->mod == ModRef)
        {
            return true;
        }
    }

    return false;
}

static void DiagMoveFromConstRef(Resolver* r, SourceRange range, const char* name)
{
    DiagErrorFmt(r->diag, range,
                 "'%s' cannot be moved as it is not owned because '%s' is a const ref. (copy() it, or bind it "
                 "with `const ref`)",
                 name, KeyRoot(r->arena, name));
}

void MarkMoved(Resolver* r, const char* name, SourceRange range)
{

    const char* root = KeyRoot(r->arena, name);

    if (IsBoxGlobalName(r, root) || IsGlobalRootKey(r, name))
    {
        DiagMoveFromGlobal(r, range, name);

        return;
    }

    if (IsConstRefRootKey(r, name))
    {
        DiagMoveFromConstRef(r, range, name);

        return;
    }

    if (StrMapGet(&r->refBoxParams, root))
    {
        DiagErrorFmt(r->diag, range, "'%s' cannot be moved as it is not owned because it is bound as a ref.", name);

        return;
    }
}

// Moving a `T?` empties the source instead of poisoning it.
void MoveOptionalSource(Resolver* r, const char* name, SourceRange range)
{
    const TypeName* type = KeyPathType(r, name);

    /* A `^T` taken into an optional slot (`Res? r = box;`) is moved, not emptied: it has no empty state. */
    if (type && !TypeNameIsOptional(type) && TypeNameIsBox(type))
    {
        MarkMoved(r, name, range);
        return;
    }

    /* Moving would empty the global behind every other function's back. */
    if (IsGlobalRootKey(r, name))
    {
        DiagMoveFromGlobal(r, range, name);

        return;
    }

    if (IsConstRefRootKey(r, name))
    {
        DiagMoveFromConstRef(r, range, name);

        return;
    }

    EmptyOptionalSource(r, name);
}

/* `drop(x)` of an optional: x is now definitely empty. */
void EmptyOptionalSource(Resolver* r, const char* name)
{

    /* The source is now definitely empty. later `path?` tests resolve to
       "definitely empty" rather than an unproven read. */
}

static bool IsModuleGlobalName(const Resolver* r, const char* name)
{
    for (size_t i = 0; i < r->mod->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->mod->globals, i);

        if (strcmp(gd->name, name) == 0)
        {
            return true;
        }
    }

    return false;
}

// Canonical index spelling for path keys; NULL when not trackable.
static char* IndexExprSpelling(Resolver* r, Node* n)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
        return ArenaFormat(r->arena, "%llu", ((IntLiteral*)n)->value);

    case NodeIdent:
    {
        const char* name = ((IdentExpr*)n)->name;

        // Globals have no visible mutation site: erase.
        return IsModuleGlobalName(r, name) ? NULL : ArenaFormat(r->arena, "%s", name);
    }

    case NodeMember:
    {
        // `.length` of a local only.
        MemberExpr* m = (MemberExpr*)n;

        if (strcmp(m->member, "length") == 0 && m->baseNode->kind == NodeIdent
            && !IsModuleGlobalName(r, ((IdentExpr*)m->baseNode)->name))
        {
            return ArenaFormat(r->arena, "%s.length", ((IdentExpr*)m->baseNode)->name);
        }

        return NULL;
    }

    case NodeIndex:
    {
        // Nested index: spell both sides.
        IndexExpr* ix = (IndexExpr*)n;
        char* baseSpell = IndexExprSpelling(r, ix->baseNode);
        char* inner = IndexExprSpelling(r, ix->index);

        if (!baseSpell || !inner)
        {
            return NULL;
        }

        return ArenaFormat(r->arena, "%s[%s]", baseSpell, inner);
    }

    case NodeUnary:
    {
        UnaryExpr* u = (UnaryExpr*)n;
        char* inner = IndexExprSpelling(r, u->operand);

        if (!inner)
        {
            return NULL;
        }

        switch (u->op)
        {
        case UnNeg:
            return ArenaFormat(r->arena, "-%s", inner);
        case UnPos:
            return ArenaFormat(r->arena, "+%s", inner);
        case UnBitNot:
            return ArenaFormat(r->arena, "~%s", inner);
        default:
            return NULL; // `!` yields a boolean, never an index.
        }
    }

    case NodeBinary:
    {
        BinaryExpr* bx = (BinaryExpr*)n;
        char* lhs = IndexExprSpelling(r, bx->lhs);
        char* rhs = IndexExprSpelling(r, bx->rhs);

        if (!lhs || !rhs)
        {
            return NULL;
        }

        const char* op = NULL;

        switch (bx->op)
        {
        case BinAdd:
            op = "+";
            break;
        case BinSub:
            op = "-";
            break;
        case BinMul:
            op = "*";
            break;
        case BinDiv:
            op = "/";
            break;
        case BinMod:
            op = "%";
            break;
        case BinBitAnd:
            op = "&";
            break;
        case BinBitOr:
            op = "|";
            break;
        case BinBitXor:
            op = "^";
            break;
        case BinShl:
            op = "<<";
            break;
        case BinShr:
            op = ">>";
            break;
        default:
            return NULL; /* comparisons/logic yield bools, not indices */
        }

        return ArenaFormat(r->arena, "(%s %s %s)", lhs, op, rhs);
    }

    default:
        return NULL;
    }
}

/* Move-tracking key for `n` (casts unwrapped); NULL if not movable. */
const char* MovableBoxSourceKey(Resolver* r, Node* n)
{
    /* Shared "what can be moved" rule (unwrap casts; ident/member/index). */
    const Node* moved = MovableBoxSourceNode(n);

    if (!moved)
    {
        return NULL;
    }

    /* Impl property reads are call-like (the getter returns a fresh value),
       never moves out of the receiver. */
    if (moved->kind == NodeMember && ((const MemberExpr*)moved)->isImplProperty)
    {
        return NULL;
    }

    if (moved->kind == NodeIdent)
    {
        return ((const IdentExpr*)moved)->name;
    }

    if (moved->kind == NodeMember)
    {
        const MemberExpr* m = (const MemberExpr*)moved;
        const char* baseKey = MovableBoxSourceKey(r, m->baseNode);

        return baseKey ? ArenaFormat(r->arena, "%s.%s", baseKey, m->member) : NULL;
    }

    if (moved->kind == NodeIndex)
    {
        const IndexExpr* ix = (const IndexExpr*)moved;
        const char* baseKey = MovableBoxSourceKey(r, ix->baseNode);

        if (!baseKey)
        {
            return NULL;
        }

        // Precise index when possible; else the erased "a[]" form.
        char* idxSpell = IndexExprSpelling(r, ix->index);

        if (idxSpell)
        {
            return ArenaFormat(r->arena, "%s[%s]", baseKey, idxSpell);
        }

        return ArenaFormat(r->arena, "%s[]", baseKey);
    }

    return NULL;
}

void FlowInit(FlowState* s)
{
    s->live = false;
}

void FlowFree(FlowState* s)
{
    s->live = false;
}

/* Snapshots the current state (dead paths are captured as not live). */
void FlowCapture(const Resolver* r, FlowState* s)
{
    s->live = !r->unreachable;
}

/* Makes `s` the current state. */
void FlowApply(Resolver* r, const FlowState* s)
{
    r->unreachable = !s->live;
}

// Control-flow join: reachable if any path in is.
void FlowJoin(FlowState* dst, const FlowState* src)
{
    dst->live |= src->live;
}

// Joins the current path into `dst` (a dead path contributes nothing).
void FlowJoinCurrent(const Resolver* r, FlowState* dst)
{
    dst->live |= !r->unreachable;
}

bool FlowSame(const FlowState* a, const FlowState* b)
{
    return a->live == b->live;
}

void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c)
{
    // Calls may rebind globals, so global facts never survive.
    for (size_t g = 0; g < r->mod->globals.count; g++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&r->mod->globals, g);

        // ...and refs into globals may dangle after any call that could reach them.
        if (!best->isExtern)
        {
        }
    }

    // A callee given a path by mutable ref (or taking it) may grow, shrink or rebind it.
    {
        size_t named = NamedParamCount(best);

        for (size_t j = 0; j < c->args.count && j < named; j++)
        {
            const ParamDecl* p = (ParamDecl*)VecGet(&best->params, j);
            Node* arg = (Node*)VecGet(&c->args, j);

            if ((p->mod == ModRef && !p->type.isConst) || (p->mod == ModNone && AliasIsOwning(r, &p->type)))
            {
            }
        }
    }

    // A typed rest collects trailing args as its element type.
    bool typedRest = best->isVariadic && !best->isCVararg;
    size_t namedCount = NamedParamCount(best);

    for (size_t j = 0; j < c->args.count; j++)
    {
        Node* arg = (Node*)VecGet(&c->args, j);

        // A non-const `ref` arg may be mutated: drop its facts.
        if (j < namedCount)
        {
            const ParamDecl* rp = (ParamDecl*)VecGet(&best->params, j);

            if (rp->mod == ModRef && !rp->type.isConst)
            {
                if (arg->kind == NodeIdent)
                {
                }

                const char* refKey = MovableBoxSourceKey(r, arg);

                if (refKey)
                {
                    // The callee may write through the ref: drop nested facts.

                    // Anything under the borrowed path may be rewritten (`h.f`, `a[0]`).

                    // A `T?` source unwrapped into a plain `ref T` param only
                    // borrows the pointee - the callee cannot rebind (or
                    // empty) the caller's slot, so the non-empty fact
                    // survives. A `ref T?` param crosses the SLOT itself,
                    // which the callee may rebind: its fact dies.
                    if (TypeNameIsOptional(&rp->type))
                    {
                    }
                }
            }
        }
        else if (typedRest && best->params.count > 0)
        {
            const ParamDecl* rp = (ParamDecl*)VecGet(&best->params, best->params.count - 1);

            if (rp->mod == ModRef && !rp->type.isConst)
            {
                if (arg->kind == NodeIdent)
                {
                }

                // A mutable ref rest element may be rewritten like a named ref arg.
            }
        }

        const TypeName* targetType = NULL;
        bool moves = false;

        if (j < namedCount)
        {
            const ParamDecl* param = (ParamDecl*)VecGet(&best->params, j);

            targetType = &param->type;
            moves = AliasIsOwning(r, &param->type) && param->mod == ModNone && !best->isExtern;
        }
        else if (typedRest)
        {
            const ParamDecl* restParam = (ParamDecl*)VecGet(&best->params, best->params.count - 1);
            const TypeName* elem = TypeNameArrayElem(&restParam->type);

            targetType = elem;
            moves = targetType && AliasIsOwning(r, targetType) && restParam->mod == ModNone && !best->isExtern;
        }

        if (!moves || !targetType)
        {
            continue;
        }

        const char* movedArgKey = MovableBoxSourceKey(r, arg);

        if (movedArgKey)
        {
            MarkMoved(r, movedArgKey, arg->range);
        }
    }
}
