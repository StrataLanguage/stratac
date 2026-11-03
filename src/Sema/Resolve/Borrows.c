/* Borrow and growth inference for array and ref parameters. */
#include "Sema/Resolve/ResolverInternal.h"

static void ScanBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed);

//-- Growable `ref T[]` parameters.
//
// A `ref T[]` argument may be a stack view: a fixed-size array, an array literal or a varargs
// rest. Growing or rebinding such a binding would realloc/free stack memory, so a parameter the
// body grows is marked `growsArray`, and call sites must hand it a real (heap) dynamic array.
static ParamDecl* RefArrayParamNamed(const FunctionDecl* f, const char* name)
{
    for (size_t i = 0; f && name && i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet((Vec*)&f->params, i);

        if (p->mod == ModRef && !p->isVarargRest && TypeNameIsDynamicArray(&p->type) && strcmp(p->name, name) == 0)
        {
            return p;
        }
    }

    return NULL;
}

/* Does any function callable as `callee` (an impl method when `isMethod`) grow its parameter `index`? */
static bool CalleeGrowsParam(const Module* mod, const char* callee, bool isMethod, size_t index)
{
    for (size_t i = 0; i < mod->functions.count; i++)
    {
        const FunctionDecl* g = (const FunctionDecl*)VecGet((Vec*)&mod->functions, i);
        const char* name = isMethod ? g->methodName : g->name;

        if (!name || strcmp(name, callee) != 0 || index >= g->params.count)
        {
            continue;
        }

        if (((const ParamDecl*)VecGet((Vec*)&g->params, index))->growsArray)
        {
            return true;
        }
    }

    return false;
}

static void MarkGrowth(FunctionDecl* f, Node* target, bool* changed)
{
    if (target && target->kind == NodeIdent)
    {
        ParamDecl* p = RefArrayParamNamed(f, ((IdentExpr*)target)->name);

        if (p && !p->growsArray)
        {
            p->growsArray = true;
            *changed = true;
        }
    }
}

static void ScanGrowth(const Module* mod, FunctionDecl* f, Node* n, bool* changed)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        for (size_t i = 0; i < ((Block*)n)->statements.count; i++)
        {
            ScanGrowth(mod, f, (Node*)VecGet(&((Block*)n)->statements, i), changed);
        }
        return;
    case NodeReturn:
        ScanGrowth(mod, f, ((ReturnStmt*)n)->value, changed);
        return;
    case NodeIf:
        ScanGrowth(mod, f, ((IfStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((IfStmt*)n)->thenBranch, changed);
        ScanGrowth(mod, f, ((IfStmt*)n)->elseBranch, changed);
        return;
    case NodeWhile:
        ScanGrowth(mod, f, ((WhileStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((WhileStmt*)n)->body, changed);
        return;
    case NodeFor:
        ScanGrowth(mod, f, ((ForStmt*)n)->init, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->condition, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->update, changed);
        ScanGrowth(mod, f, ((ForStmt*)n)->body, changed);
        return;
    case NodeVarDecl:
        ScanGrowth(mod, f, ((VarDeclStmt*)n)->init, changed);
        return;
    case NodeExprStmt:
        ScanGrowth(mod, f, ((ExprStmt*)n)->expr, changed);
        return;
    case NodeDefer:
        ScanGrowth(mod, f, ((DeferStmt*)n)->stmt, changed);
        return;
    case NodeUnary:
        ScanGrowth(mod, f, ((UnaryExpr*)n)->operand, changed);
        return;
    case NodeBinary:
        ScanGrowth(mod, f, ((BinaryExpr*)n)->lhs, changed);
        ScanGrowth(mod, f, ((BinaryExpr*)n)->rhs, changed);
        return;
    case NodeAssign:
        MarkGrowth(f, ((AssignExpr*)n)->target, changed);
        ScanGrowth(mod, f, ((AssignExpr*)n)->value, changed);
        return;
    case NodeIncDec:
        ScanGrowth(mod, f, ((IncDecExpr*)n)->operand, changed);
        return;
    case NodeCast:
        ScanGrowth(mod, f, ((CastExpr*)n)->operand, changed);
        return;
    case NodeMember:
        ScanGrowth(mod, f, ((MemberExpr*)n)->baseNode, changed);
        return;
    case NodeIndex:
        ScanGrowth(mod, f, ((IndexExpr*)n)->baseNode, changed);
        ScanGrowth(mod, f, ((IndexExpr*)n)->index, changed);
        return;
    case NodeNullTest:
        ScanGrowth(mod, f, ((NullTestExpr*)n)->operand, changed);
        return;
    case NodeConditional:
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->condition, changed);
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->thenExpr, changed);
        ScanGrowth(mod, f, ((ConditionalExpr*)n)->elseExpr, changed);
        return;
    case NodeArrayInit:
        for (size_t i = 0; i < ((ArrayInitExpr*)n)->elements.count; i++)
        {
            ScanGrowth(mod, f, (Node*)VecGet(&((ArrayInitExpr*)n)->elements, i), changed);
        }
        ScanGrowth(mod, f, ((ArrayInitExpr*)n)->repeatCount, changed);
        return;
    case NodeStructInit:
        for (size_t i = 0; i < ((StructInitExpr*)n)->fields.count; i++)
        {
            ScanGrowth(mod, f, ((StructInitField*)VecGet(&((StructInitExpr*)n)->fields, i))->value, changed);
        }
        return;
    case NodeCall:
    {
        CallExpr* c = (CallExpr*)n;
        bool isMethod = c->calleeBase != NULL;

        ScanGrowth(mod, f, c->calleeBase, changed);

        if (c->callee && c->args.count > 0 && (ArrayBuiltinFlagsOf(c->callee) & ABF_RESIZES))
        {
            MarkGrowth(f, (Node*)VecGet(&c->args, 0), changed);
        }

        /* A receiver passed as a method's `self`. */
        if (isMethod && c->callee && CalleeGrowsParam(mod, c->callee, true, 0))
        {
            MarkGrowth(f, c->calleeBase, changed);
        }

        for (size_t i = 0; i < c->args.count; i++)
        {
            Node* arg = (Node*)VecGet(&c->args, i);

            if (c->callee && CalleeGrowsParam(mod, c->callee, isMethod, isMethod ? i + 1 : i))
            {
                MarkGrowth(f, arg, changed);
            }

            ScanGrowth(mod, f, arg, changed);
        }
        return;
    }
    default:
        return;
    }
}

//-- Read-only by-value parameters borrow.
//
// A by-value `string`, `T[]` or owning-struct parameter would take its argument (move it out of
// the caller). When the body never consumes it - never moves it or a part of it anywhere, never
// writes to it, never passes it on to something that does - it is passed as `const ref` instead,
// so callers keep their value. Decided before resolution, to a fixpoint over calls; a miss shows
// up as a const error in the body, never as a wrong program.
static bool BorrowCandidateType(Resolver* r, const TypeName* t)
{
    if (TypeNameIsBox(t) || TypeNameIsOptional(t) || TypeNameIsFixedArray(t))
    {
        return false;
    }

    const char* name = TypeRegistryResolveAlias(&r->m_registry, t->name);

    return TypeIsString(&r->m_registry, t->name) || TypeNameIsDynamicArray(t)
           || (name && TypeRegistryIsOwningStruct(&r->m_registry, name) && !TypeNameHasDrop(&r->m_registry, t));
}

/* The candidate parameter at the root of a path (`p`, `p.a[i].b`), or NULL. */
static ParamDecl* BorrowRoot(FunctionDecl* f, Node* n)
{
    while (n && (n->kind == NodeMember || n->kind == NodeIndex || n->kind == NodeCast))
    {
        n = n->kind == NodeMember  ? ((MemberExpr*)n)->baseNode
            : n->kind == NodeIndex ? ((IndexExpr*)n)->baseNode
                                   : ((CastExpr*)n)->operand;
    }

    if (!n || n->kind != NodeIdent)
    {
        return NULL;
    }

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);

        if (p->borrowCandidate && strcmp(p->name, ((IdentExpr*)n)->name) == 0)
        {
            return p;
        }
    }

    return NULL;
}

/* The declared type at the end of a path rooted at a parameter; NULL when it is not an owning value
   (a scalar, `.length`, ...). Unknown shapes count as owning. */
static const TypeName* BorrowPathType(Resolver* r, ParamDecl* root, Node* n)
{
    if (n->kind == NodeIdent)
    {
        return &root->type;
    }

    if (n->kind == NodeCast)
    {
        return &((CastExpr*)n)->type; /* `(Alias)p` retags; `(cstring)p` only views */
    }

    const TypeName* t = NULL;

    if (n->kind == NodeIndex)
    {
        t = BorrowPathType(r, root, ((IndexExpr*)n)->baseNode);
        return t ? (TypeNameArrayElem(t) ? TypeNameArrayElem(t) : NULL) : NULL;
    }

    if (n->kind != NodeMember)
    {
        return &root->type;
    }

    t = BorrowPathType(r, root, ((MemberExpr*)n)->baseNode);

    if (!t)
    {
        return NULL;
    }

    if (TypeNameIsBox(t) || TypeNameIsOptional(t))
    {
        t = TypeNameBoxInner(t);
    }

    const char* member = ((MemberExpr*)n)->member;
    const char* name = t ? TypeRegistryResolveAlias(&r->m_registry, t->name) : NULL;
    const StructType* st = name ? TypeRegistryFind(&r->m_registry, name) : NULL;

    if (!st)
    {
        return NULL; /* `.length`, `.data`, swizzles: not owning */
    }

    for (size_t i = 0; i < st->fields.count; i++)
    {
        const FieldDecl* fd = (const FieldDecl*)VecGet((Vec*)&st->fields, i);

        if (strcmp(fd->name, member) == 0)
        {
            return &fd->type;
        }
    }

    return &root->type;
}

static void ConsumeParam(ParamDecl* p, bool* changed)
{
    if (p && p->borrowCandidate)
    {
        p->borrowCandidate = false;
        *changed = true;
    }
}

/* `n` is used where an owning value is taken (moved): consumes its root if it is an owning path. */
static void ConsumeIfTaken(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ParamDecl* p = BorrowRoot(f, n);

    if (p)
    {
        const TypeName* t = BorrowPathType(r, p, n);

        if (t && AliasIsOwningValue(r, t))
        {
            ConsumeParam(p, changed);
        }
    }
}

/* An argument passed to parameter `index` of whatever `callee` names. */
static void ScanBorrowArg(Resolver* r, FunctionDecl* f, CallExpr* c, Node* arg, size_t index, bool isMethod,
                          bool* changed)
{
    ParamDecl* root = BorrowRoot(f, arg);

    if (!root)
    {
        return;
    }

    bool found = false;

    for (size_t i = 0; i < r->m_mod->functions.count; i++)
    {
        const FunctionDecl* g = (const FunctionDecl*)VecGet(&r->m_mod->functions, i);
        const char* name = isMethod ? g->methodName : g->name;

        if (!name || strcmp(name, c->callee) != 0)
        {
            continue;
        }

        found = true;

        if (index >= g->params.count)
        {
            if (!g->isCVararg)
            {
                ConsumeIfTaken(r, f, arg, changed); /* a typed rest takes its elements */
            }
            continue;
        }

        const ParamDecl* q = (const ParamDecl*)VecGet((Vec*)&g->params, index);

        if (q->mod == ModRef && !q->type.isConst)
        {
            ConsumeParam(root, changed); /* written through */
        }
        else if (q->mod == ModNone && !g->isExtern && !q->borrowCandidate && !q->inferredBorrow)
        {
            ConsumeIfTaken(r, f, arg, changed); /* taken */
        }
    }

    if (!found)
    {
        ConsumeParam(root, changed); /* a delegate, or something unknown */
    }
}

static void ScanBlockBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    for (size_t i = 0; i < ((Block*)n)->statements.count; i++)
    {
        ScanBorrows(r, f, (Node*)VecGet(&((Block*)n)->statements, i), changed);
    }
}

static void ScanReturnBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ConsumeIfTaken(r, f, ((ReturnStmt*)n)->value, changed);
    ScanBorrows(r, f, ((ReturnStmt*)n)->value, changed);
}

static void ScanIfBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((IfStmt*)n)->condition, changed);
    ScanBorrows(r, f, ((IfStmt*)n)->thenBranch, changed);
    ScanBorrows(r, f, ((IfStmt*)n)->elseBranch, changed);
}

static void ScanWhileBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((WhileStmt*)n)->condition, changed);
    ScanBorrows(r, f, ((WhileStmt*)n)->body, changed);
}

static void ScanForBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((ForStmt*)n)->init, changed);
    ScanBorrows(r, f, ((ForStmt*)n)->condition, changed);
    ScanBorrows(r, f, ((ForStmt*)n)->update, changed);
    ScanBorrows(r, f, ((ForStmt*)n)->body, changed);
}

static void ScanVarDeclBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    VarDeclStmt* vd = (VarDeclStmt*)n;

    if (vd->isRef)
    {
        if (!vd->type.isConst)
        {
            ConsumeParam(BorrowRoot(f, vd->init), changed); /* a mutable alias */
        }
    }
    else
    {
        ConsumeIfTaken(r, f, vd->init, changed);
    }
    ScanBorrows(r, f, vd->init, changed);
}

static void ScanBinaryBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((BinaryExpr*)n)->lhs, changed);
    ScanBorrows(r, f, ((BinaryExpr*)n)->rhs, changed);
}

static void ScanAssignBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ConsumeParam(BorrowRoot(f, ((AssignExpr*)n)->target), changed);
    ConsumeIfTaken(r, f, ((AssignExpr*)n)->value, changed);
    ScanBorrows(r, f, ((AssignExpr*)n)->target, changed);
    ScanBorrows(r, f, ((AssignExpr*)n)->value, changed);
}

static void ScanIncDecBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ConsumeParam(BorrowRoot(f, ((IncDecExpr*)n)->operand), changed);
    ScanBorrows(r, f, ((IncDecExpr*)n)->operand, changed);
}

static void ScanIndexBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((IndexExpr*)n)->baseNode, changed);
    ScanBorrows(r, f, ((IndexExpr*)n)->index, changed);
}

static void ScanConditionalBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ScanBorrows(r, f, ((ConditionalExpr*)n)->condition, changed);
    ScanBorrows(r, f, ((ConditionalExpr*)n)->thenExpr, changed);
    ScanBorrows(r, f, ((ConditionalExpr*)n)->elseExpr, changed);
}

static void ScanArrayInitBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    ArrayInitExpr* ai = (ArrayInitExpr*)n;

    for (size_t i = 0; i < ai->elements.count; i++)
    {
        Node* e = (Node*)VecGet(&ai->elements, i);

        if (!ai->repeatCount)
        {
            ConsumeIfTaken(r, f, e, changed); /* `{ v; n }` copies its value */
        }
        ScanBorrows(r, f, e, changed);
    }
    ScanBorrows(r, f, ai->repeatCount, changed);
}

static void ScanStructInitBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    for (size_t i = 0; i < ((StructInitExpr*)n)->fields.count; i++)
    {
        Node* v = ((StructInitField*)VecGet(&((StructInitExpr*)n)->fields, i))->value;
        ConsumeIfTaken(r, f, v, changed);
        ScanBorrows(r, f, v, changed);
    }
}

static void ScanCallBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    CallExpr* c = (CallExpr*)n;
    bool isMethod = c->calleeBase != NULL;

    ScanBorrows(r, f, c->calleeBase, changed);

    for (size_t i = 0; i < c->args.count; i++)
    {
        ScanBorrows(r, f, (Node*)VecGet(&c->args, i), changed);
    }

    if (!c->callee)
    {
        for (size_t i = 0; i < c->args.count; i++)
        {
            ConsumeParam(BorrowRoot(f, (Node*)VecGet(&c->args, i)), changed);
        }
        return;
    }

    Node* arg0 = c->args.count > 0 ? (Node*)VecGet(&c->args, 0) : NULL;

    if (!isMethod
        && (IsArrayMutator(c->callee) || strcmp(c->callee, "drop") == 0 || strcmp(c->callee, "addressof") == 0))
    {
        ConsumeParam(BorrowRoot(f, arg0), changed);

        if (strcmp(c->callee, "array_push") == 0 && c->args.count > 1)
        {
            ConsumeIfTaken(r, f, (Node*)VecGet(&c->args, 1), changed);
        }

        /* `array_replace(a, i, v)` stores `v` in the array, so `v` is owned, not borrowed. */
        if (strcmp(c->callee, "array_replace") == 0 && c->args.count > 2)
        {
            ConsumeParam(BorrowRoot(f, (Node*)VecGet(&c->args, 2)), changed);
        }
        return;
    }

    if (!isMethod
        && (strcmp(c->callee, "copy") == 0 || strcmp(c->callee, "sizeof") == 0 || strcmp(c->callee, "alignof") == 0
            || strcmp(c->callee, "load") == 0 || strcmp(c->callee, "substring") == 0))
    {
        return; /* only read */
    }

    if (isMethod)
    {
        ScanBorrowArg(r, f, c, c->calleeBase, 0, true, changed);
    }

    for (size_t i = 0; i < c->args.count; i++)
    {
        ScanBorrowArg(r, f, c, (Node*)VecGet(&c->args, i), isMethod ? i + 1 : i, isMethod, changed);
    }
}

static void ScanBorrows(Resolver* r, FunctionDecl* f, Node* n, bool* changed)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        ScanBlockBorrows(r, f, n, changed);
        return;
    case NodeReturn:
        ScanReturnBorrows(r, f, n, changed);
        return;
    case NodeIf:
        ScanIfBorrows(r, f, n, changed);
        return;
    case NodeWhile:
        ScanWhileBorrows(r, f, n, changed);
        return;
    case NodeFor:
        ScanForBorrows(r, f, n, changed);
        return;
    case NodeVarDecl:
        ScanVarDeclBorrows(r, f, n, changed);
        return;
    case NodeExprStmt:
        ScanBorrows(r, f, ((ExprStmt*)n)->expr, changed);
        return;
    case NodeDefer:
        ScanBorrows(r, f, ((DeferStmt*)n)->stmt, changed);
        return;
    case NodeUnary:
        ScanBorrows(r, f, ((UnaryExpr*)n)->operand, changed);
        return;
    case NodeBinary:
        ScanBinaryBorrows(r, f, n, changed);
        return;
    case NodeAssign:
        ScanAssignBorrows(r, f, n, changed);
        return;
    case NodeIncDec:
        ScanIncDecBorrows(r, f, n, changed);
        return;
    case NodeCast:
        ScanBorrows(r, f, ((CastExpr*)n)->operand, changed);
        return;
    case NodeMember:
        ScanBorrows(r, f, ((MemberExpr*)n)->baseNode, changed);
        return;
    case NodeIndex:
        ScanIndexBorrows(r, f, n, changed);
        return;
    case NodeNullTest:
        ScanBorrows(r, f, ((NullTestExpr*)n)->operand, changed);
        return;
    case NodeConditional:
        ScanConditionalBorrows(r, f, n, changed);
        return;
    case NodeArrayInit:
        ScanArrayInitBorrows(r, f, n, changed);
        return;
    case NodeStructInit:
        ScanStructInitBorrows(r, f, n, changed);
        return;
    case NodeCall:
        ScanCallBorrows(r, f, n, changed);
        return;
    default:
        return;
    }
}

void InferBorrowedParams(Resolver* r)
{
    Module* mod = r->m_mod;

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);
        bool eligible = f->body && !f->isExtern && !f->isExport && !f->isDelegate;

        for (size_t j = 0; j < f->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&f->params, j);
            p->borrowCandidate
                = eligible && p->mod == ModNone && !p->isVarargRest && !p->isReturn && BorrowCandidateType(r, &p->type);
        }
    }

    bool changed = true;

    for (int round = 0; changed && round < 64; round++)
    {
        changed = false;

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);

            if (f->body)
            {
                ScanBorrows(r, f, f->body, &changed);
            }
        }
    }

    for (size_t i = 0; i < mod->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);

        for (size_t j = 0; j < f->params.count; j++)
        {
            ParamDecl* p = (ParamDecl*)VecGet(&f->params, j);

            if (p->borrowCandidate)
            {
                p->borrowCandidate = false;
                p->inferredBorrow = true;
                p->mod = ModRef;
                p->type.isConst = true;
            }
        }
    }
}

/* Marks every growing `ref T[]` parameter, to a fixpoint: passing a parameter on to a callee that
   grows it grows it too. */
void MarkGrowableArrayParams(Module* mod)
{
    bool changed = true;

    for (int round = 0; changed && round < 64; round++)
    {
        changed = false;

        for (size_t i = 0; i < mod->functions.count; i++)
        {
            FunctionDecl* f = (FunctionDecl*)VecGet(&mod->functions, i);
            ScanGrowth(mod, f, f->body, &changed);
        }
    }
}

bool IsGrowableArrayParam(const Resolver* r, const char* name)
{
    const ParamDecl* p = RefArrayParamNamed(r->m_currentFunction, name);

    return p && p->growsArray;
}

/* A growing `ref T[]` parameter needs a heap array: reject stack views at the call site. */
void CheckGrowableArrayArgs(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope)
{
    for (size_t i = 0; i < best->params.count && i < c->args.count; i++)
    {
        const ParamDecl* p = (const ParamDecl*)VecGet((Vec*)&best->params, i);

        if (!p->growsArray)
        {
            continue;
        }

        Node* arg = (Node*)VecGet(&c->args, i);
        const TypeName* argType = InferType(r, arg, scope);
        const char* why = NULL;

        if (arg->kind == NodeArrayInit)
        {
            why = "an array literal";
        }
        else if (argType && TypeNameIsFixedArray(argType))
        {
            why = "a fixed-size array";
        }
        else if (arg->kind == NodeIdent && r->m_currentFunction)
        {
            for (size_t j = 0; j < r->m_currentFunction->params.count; j++)
            {
                const ParamDecl* own = (const ParamDecl*)VecGet((Vec*)&r->m_currentFunction->params, j);

                if (own->isVarargRest && strcmp(own->name, ((IdentExpr*)arg)->name) == 0)
                {
                    why = "a varargs rest array";
                }
            }
        }

        if (why)
        {
            DiagErrorFmt(r->m_diag, arg->range,
                         "'%s' grows or rebinds its parameter '%s', so it needs a dynamic array variable, not %s",
                         best->methodName ? best->methodName : best->name, p->name, why);
        }
    }
}
