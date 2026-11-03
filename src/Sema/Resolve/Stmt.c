/* Statement walking, scopes, defer handling and loop flow. */
#include "Sema/Resolve/ResolverInternal.h"

static void WalkLoop(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope);
static void ScopePush(Resolver* r);
static void ScopePop(Resolver* r, StrMap* scope);
static void WalkStmt(Resolver* r, Node* n, StrMap* scope);
static bool ExprIsConstantTrue(const Node* n);
static bool ExprIsConstantFalse(const Node* n);

// Reports each diagnostic of `src` that `dst` does not hold yet.
static void ForwardNewDiagnostics(DiagnosticEngine* dst, const DiagnosticEngine* src)
{
    for (size_t i = 0; i < src->m_count; i++)
    {
        const Diagnostic* d = &src->m_diagnostics[i];
        bool seen = false;

        for (size_t j = 0; j < dst->m_count && !seen; j++)
        {
            const Diagnostic* e = &dst->m_diagnostics[j];

            seen = e->severity == d->severity && e->range.start == d->range.start && e->range.length == d->range.length
                   && e->range.fileId == d->range.fileId && strcmp(e->message, d->message) == 0;
        }

        if (!seen)
        {
            DiagReport(dst, d->severity, d->range, d->message);
        }
    }
}

/* Walks a deferred statement at one of its exits. Names declared after the
   `defer` are hidden, and repeats of a diagnostic from another exit are
   dropped. */
static void WalkDeferred(Resolver* r, DeferEntry* e, StrMap* scope)
{
    Vec hidden;
    VecInit(&hidden);

    for (size_t f = e->m_frame; f < r->m_scopeDecls.count; f++)
    {
        Vec* frame = (Vec*)VecGet(&r->m_scopeDecls, f);

        for (size_t k = f == e->m_frame ? e->m_declMark : 0; k < frame->count; k++)
        {
            const char* name = (const char*)VecGet(frame, k);

            VecPush(&hidden, (void*)name);
            VecPush(&hidden, StrMapGet(scope, name));
            StrMapPut(scope, name, NULL);
        }
    }

    DiagnosticEngine scratch;
    DiagnosticEngineInit(&scratch);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &scratch;

    bool wasUnreachable = r->m_unreachable;
    r->m_deferWalkDepth++;

    WalkStmt(r, e->m_stmt, scope);

    r->m_deferWalkDepth--;
    r->m_unreachable = wasUnreachable;
    r->m_diag = realDiag;

    ForwardNewDiagnostics(r->m_diag, &scratch);
    DiagnosticEngineFree(&scratch);

    for (size_t i = hidden.count; i >= 2; i -= 2)
    {
        StrMapPut(scope, (const char*)VecGet(&hidden, i - 2), VecGet(&hidden, i - 1));
    }

    free(hidden.items);

    e->m_ranAtExit = true;
}

// Runs one scope's deferred statements, last `defer` first.
static void RunDeferFrame(Resolver* r, Vec* frame, StrMap* scope)
{
    for (size_t i = frame->count; i-- > 0;)
    {
        WalkDeferred(r, (DeferEntry*)VecGet(frame, i), scope);
    }
}

// Runs the deferred statements of scopes [fromIndex, top), innermost first.
static void RunDefersFrom(Resolver* r, StackMark fromIndex, StrMap* scope)
{
    for (size_t f = r->m_deferFrames.count; f-- > fromIndex;)
    {
        RunDeferFrame(r, (Vec*)VecGet(&r->m_deferFrames, f), scope);
    }
}

// Records a `defer` on the innermost scope. Its checks happen at the exits.
static void QueueDefer(Resolver* r, DeferStmt* d, StrMap* scope)
{
    if (r->m_deferFrames.count == 0 || r->m_scopeDecls.count == 0)
    {
        WalkStmt(r, d->stmt, scope);
        return;
    }

    DeferEntry* e = (DeferEntry*)arena_alloc(r->m_arena, sizeof(DeferEntry));
    e->m_stmt = d->stmt;
    e->m_frame = r->m_scopeDecls.count - 1;
    e->m_declMark = ((Vec*)VecGet(&r->m_scopeDecls, e->m_frame))->count;
    e->m_ranAtExit = false;

    /* Resolve it here, in the scope it was written in, but keep the flow
       state untouched: nothing in it has run yet. The diagnostics are only
       used if no reachable exit ever runs the statement. */
    FlowState saved;
    FlowInit(&saved);
    FlowCapture(r, &saved);

    DiagnosticEngineInit(&e->m_early);
    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &e->m_early;
    r->m_deferWalkDepth++;

    WalkStmt(r, d->stmt, scope);

    r->m_deferWalkDepth--;
    r->m_diag = realDiag;

    r->m_unreachable = !saved.m_live;
    FlowFree(&saved);

    VecPush((Vec*)VecGet(&r->m_deferFrames, r->m_deferFrames.count - 1), e);
}

/* `break`/`continue`: run the defers of the scopes being left, then hand the
   state to the loop's exit or back edge. The path after it is dead. */
static void WalkLoopJump(Resolver* r, StrMap* scope, bool isBreak, SourceRange range)
{
    LoopFlow* lf = r->m_loopFlows.count > 0 ? (LoopFlow*)VecGet(&r->m_loopFlows, r->m_loopFlows.count - 1) : NULL;

    /* A deferred statement runs at scope exit, wherever that is: it cannot also jump out of a loop. */
    if (r->m_deferWalkDepth > 0 && (!lf || lf->m_deferWalkDepth != r->m_deferWalkDepth))
    {
        DiagErrorFmt(r->m_diag, range, "'%s' cannot leave a defer block", isBreak ? "break" : "continue");
    }

    /* A jump out of a deferred statement is not modeled; it only ends the path. */
    if (lf && lf->m_deferWalkDepth == r->m_deferWalkDepth && !r->m_unreachable)
    {
        RunDefersFrom(r, lf->m_deferDepth, scope);
        FlowJoinCurrent(r, isBreak ? &lf->m_breaks : &lf->m_continues);
    }

    r->m_unreachable = true;
}

/* `if`/`while`/`for` conditions must be `bool`: no implicit truthiness for
   numerics (`if (x)` on a float is a != 0.0 test, rarely what was meant). */
void CheckBoolCondition(Resolver* r, Node* cond, StrMap* scope)
{
    const TypeName* t = cond ? InferType(r, cond, scope) : NULL;

    if (t && !IsBoolContextType(&r->m_registry, t))
    {
        DiagErrorFmt(r->m_diag, cond->range, "condition must be of type 'bool', found '%s'", t->name);
    }
}

/* One trip around the loop from the current (loop head) state: condition,
   body, continue join, update. `exit` receives the condition-false and break
   states, `back` the state flowing back to the head. */
static void WalkLoopIteration(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope,
                              FlowState* exit, FlowState* back)
{
    LoopFlow lf;
    FlowInit(&lf.m_breaks);
    FlowInit(&lf.m_continues);
    lf.m_deferDepth = r->m_deferFrames.count;
    lf.m_deferWalkDepth = r->m_deferWalkDepth;

    if (condition)
    {
        ResolveExpr(r, condition, scope);
        CheckBoolCondition(r, condition, scope);
    }

    // Condition false: leave the loop with the opposite facts.
    if (condition && !ExprIsConstantTrue(condition))
    {
        FlowState afterCond;
        FlowInit(&afterCond);
        FlowCapture(r, &afterCond);

        FlowJoinCurrent(r, exit);
        FlowApply(r, &afterCond);
        FlowFree(&afterCond);
    }

    // Condition true: its facts hold through the body.
    if (condition)
    {
    }

    VecPush(&r->m_loopFlows, &lf);

    if (bodyScope)
    {
        ScopePush(r);
    }

    WalkStmt(r, body, scope);

    if (bodyScope)
    {
        ScopePop(r, scope);
    }

    VecPop(&r->m_loopFlows);

    // Falling off the body end and every `continue` meet before the update.
    FlowJoinCurrent(r, &lf.m_continues);
    FlowApply(r, &lf.m_continues);

    if (update)
    {
        ResolveExpr(r, update, scope);
    }

    FlowCapture(r, back);
    FlowJoin(exit, &lf.m_breaks);

    FlowFree(&lf.m_breaks);
    FlowFree(&lf.m_continues);
}

/* Loops iterate muted until the head state (entry joined with every back
   edge) is stable, then walk once more from that state for diagnostics. The
   state after the loop joins the condition-false exit with every `break`. */
static void WalkLoop(Resolver* r, Node* condition, Node* update, Node* body, bool bodyScope, StrMap* scope)
{
    FlowState head;
    FlowInit(&head);
    FlowCapture(r, &head);

    DiagnosticEngine warmup;
    DiagnosticEngineInit(&warmup);

    DiagnosticEngine* realDiag = r->m_diag;
    r->m_diag = &warmup;

    for (int pass = 0; pass < LOOP_MAX_PASSES; pass++)
    {
        FlowState exit;
        FlowState back;
        FlowInit(&exit);
        FlowInit(&back);

        FlowApply(r, &head);
        WalkLoopIteration(r, condition, update, body, bodyScope, scope, &exit, &back);

        FlowState next;
        FlowInit(&next);
        FlowJoin(&next, &head);
        FlowJoin(&next, &back);

        bool stable = FlowSame(&next, &head);

        FlowFree(&head);
        head = next;

        FlowFree(&exit);
        FlowFree(&back);

        if (stable)
        {
            break;
        }
    }

    r->m_diag = realDiag;
    DiagnosticEngineFree(&warmup);

    FlowState exit;
    FlowState back;
    FlowInit(&exit);
    FlowInit(&back);

    FlowApply(r, &head);
    WalkLoopIteration(r, condition, update, body, bodyScope, scope, &exit, &back);
    FlowApply(r, &exit);

    FlowFree(&head);
    FlowFree(&exit);
    FlowFree(&back);
}

/**
 * @brief Pushes a new scope to the scopeDecls stack.
 */
static void ScopePush(Resolver* r)
{
    Vec* frame = (Vec*)arena_alloc(r->m_arena, sizeof(Vec));
    VecInit(frame);
    VecPush(&r->m_scopeDecls, frame);

    Vec* defers = (Vec*)arena_alloc(r->m_arena, sizeof(Vec));
    VecInit(defers);
    VecPush(&r->m_deferFrames, defers);
}

/**
 * @brief Runs the scope's defers (when its end is reachable) and invalidates all symbols in it.
 */
static void ScopePop(Resolver* r, StrMap* scope)
{
    if (r->m_deferFrames.count > 0)
    {
        Vec* defers = (Vec*)VecPop(&r->m_deferFrames);

        if (!r->m_unreachable)
        {
            RunDeferFrame(r, defers, scope);
        }

        for (size_t i = 0; i < defers->count; i++)
        {
            DeferEntry* e = (DeferEntry*)VecGet(defers, i);

            /* No reachable exit ran it: keep what the `defer` site found. */
            if (!e->m_ranAtExit)
            {
                ForwardNewDiagnostics(r->m_diag, &e->m_early);
            }

            DiagnosticEngineFree(&e->m_early);
        }

        free(defers->items);
    }

    if (r->m_scopeDecls.count == 0)
    {
        return;
    }

    Vec* frame = (Vec*)VecPop(&r->m_scopeDecls);

    for (size_t i = 0; i < frame->count; i++)
    {
        const char* name = (const char*)VecGet(frame, i);
        StrMapPut(scope, name, NULL);
    }
}

/**
 * @brief Declares a new symbol `name` in the scope `scope`.
 */
void ScopeDeclare(Resolver* r, StrMap* scope, const char* name, void* typePtr, SourceRange range)
{
    void* existing = StrMapGet(scope, name);

    if (existing && existing != typePtr)
    {
        DiagErrorFmt(r->m_diag, range, "redefinition of '%s'", name);
    }

    StrMapPut(scope, name, typePtr);

    if (r->m_scopeDecls.count > 0)
    {
        Vec* top = (Vec*)VecGet(&r->m_scopeDecls, r->m_scopeDecls.count - 1);
        VecPush(top, (void*)name);
    }
}

/* Unknown types must fail here so codegen is skipped. Returns false after reporting. */
static bool CheckLocalTypeKnown(Resolver* r, VarDeclStmt* vd)
{
    const TypeName* t = &vd->type;
    while (t && (TypeNameIsBox(t) || TypeNameIsOptional(t) || TypeNameHasArrayShape(t)))
    {
        t = (TypeNameIsBox(t) || TypeNameIsOptional(t)) ? t->inner : t->elem;
    }
    if (t && t->name && !IsStringType(t->primitiveType) && !IsCStringType(t->primitiveType)
        && !IsScalarType(t->primitiveType) && !IsSimdVector(t->primitiveType)
        && !TypeRegistryIsUserType(&r->m_registry, t->name))
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "unknown type '%s'", t->name);
        return false;
    }
    return true;
}

static void CheckLocalDeclTypes(Resolver* r, VarDeclStmt* vd)
{
    if (IsIncompleteStruct(&r->m_registry, vd->type.name))
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, vd->type.name);
    }

    /* `T?` of an alias of an optional is an optional of an optional, which `T??` is not allowed to be. */
    if (TypeNameIsOptional(&vd->type) && vd->type.inner)
    {
        const TypeName* innerResolved = ResolveAliasedType(&r->m_registry, r->m_arena, vd->type.inner);

        if (innerResolved && TypeNameIsOptional(innerResolved))
        {
            DiagErrorFmt(r->m_diag, vd->base.range, "'%s' is an optional of an optional ('%s' already may be empty)",
                         vd->name, vd->type.inner->name);
        }
    }

    /* An array of an incomplete struct has elements of unknown size. */
    for (const TypeName* el = vd->type.name ? &vd->type : NULL; el && TypeNameHasArrayShape(el);)
    {
        el = el->elem;

        if (el && !TypeNameHasArrayShape(el) && !TypeNameIsBox(el) && !TypeNameIsOptional(el)
            && IsIncompleteStruct(&r->m_registry, el->name))
        {
            DiagErrorFmt(r->m_diag, vd->base.range,
                         "'%s' is an array of incomplete type '%s'; its element size is unknown", vd->name, el->name);
            break;
        }
    }
}

/* Shape checks for a fixed-size array local. Reports and returns false on the first problem. */
static bool CheckFixedArrayLocal(Resolver* r, VarDeclStmt* vd, const TypeName** leafOut, ArrayLength* totalOut)
{
    if (!(TypeNameHasArrayShape(&vd->type) && vd->type.length >= 0))
    {
        /* e.g. `int[][4] x;` — a dynamic array of fixed arrays. */
        DiagErrorFmt(r->m_diag, vd->base.range,
                     "local variable '%s' has invalid fixed-size array type ('%s'); "
                     "fixed-size arrays may only be declared as 'T[N]' locals",
                     vd->name, vd->type.name);
        return false;
    }

    if (vd->type.length < 1)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "fixed-size array local '%s' must have a length of at least 1",
                     vd->name);
        return false;
    }

    ArrayLength total = 0;
    const TypeName* leaf = FixedArrayLeaf(&vd->type, &total);

    if (TypeNameHasArrayShape(leaf))
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "fixed-size array local '%s' may not contain a dynamic array",
                     vd->name);
        return false;
    }

    if (TypeNameIsBox(leaf) || AliasIsOwning(r, leaf))
    {
        DiagErrorFmt(r->m_diag, vd->base.range,
                     "fixed-size array local '%s' may not own its elements ('%s' is owning); "
                     "fixed-size arrays have no drop glue",
                     vd->name, leaf->name);
        return false;
    }

    if (TypeRegistryIsOwningStruct(&r->m_registry, leaf->name))
    {
        DiagErrorFmt(r->m_diag, vd->base.range,
                     "fixed-size array local '%s' may not contain an owning struct ('%s'); "
                     "fixed-size arrays have no drop glue",
                     vd->name, leaf->name);
        return false;
    }

    if (IsIncompleteStruct(&r->m_registry, leaf->name))
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "variable '%s' has incomplete type '%s'", vd->name, leaf->name);
        return false;
    }

    if (!vd->init || vd->init->kind != NodeArrayInit)
    {
        DiagErrorFmt(r->m_diag, vd->base.range,
                     "fixed-size array local '%s' must be initialized with a braced list "
                     "('{ ... }'), e.g. 'float %s[%ld] = { ... }'",
                     vd->name, vd->name, vd->type.length);
        return false;
    }
    *leafOut = leaf;
    *totalOut = total;
    return true;
}

/* `{ value; N }`: every element (of every row) is the value; N must be the full length. */
static void WalkFixedArrayRepeatInit(Resolver* r, VarDeclStmt* vd, ArrayInitExpr* fixedInit, const TypeName* leaf,
                                     ArrayLength total, StrMap* scope)
{
    fixedInit->elementType = leaf;
    ResolveExpr(r, fixedInit->repeatCount, scope);

    ConstUInt repeat = 0;
    bool negative = false;

    if (!ResolvedConstIntValue(fixedInit->repeatCount, &repeat, &negative) || negative || repeat != (ConstUInt)total)
    {
        DiagErrorFmt(r->m_diag, fixedInit->repeatCount->range,
                     "the count in '{ value; count }' for fixed-size array local '%s' must be the "
                     "constant %ld (its length)",
                     vd->name, total);
    }

    Node* value = (Node*)VecGet(&fixedInit->elements, 0);

    if (value->kind == NodeArrayInit || value->kind == NodeStructInit)
    {
        value = ApplyBracedStructTarget(r, value, leaf);
        VecSet(&fixedInit->elements, 0, value);
    }

    ResolveExpr(r, value, scope);

    const TypeName* valueType = InferType(r, value, scope);

    if (valueType && !IsAssignableType(r, leaf, valueType) && !IsStrLiteralForCString(r, leaf, value))
    {
        DiagErrorFmt(r->m_diag, value->range, "a '%s' cannot fill the '%s' elements of local '%s'", valueType->name,
                     leaf->name, vd->name);
    }
}

static bool FlattenFixedArrayInit(Resolver* r, VarDeclStmt* vd, ArrayInitExpr* fixedInit, const TypeName* leaf,
                                  ArrayLength total)
{
    /* Loop bodies walk twice (muted warmup + real); a multidim init
       flattened by the first pass must not fail the shape check on
       the second. An already-flat init is the output of this very
       processing — detect it BEFORE stamping elementType below (a
       user-written flat multidim init still carries the parser's
       intermediate element type, so it is correctly NOT skipped and
       fails the shape check). Error paths never mutate the init, so
       re-walks reproduce their diagnostics loudly instead of
       converging silent. */
    bool alreadyFlat = fixedInit->elementType == leaf && fixedInit->elements.count == (size_t)total
                       && !HasNestedArrayInit(fixedInit);

    if (!alreadyFlat)
    {
        if (!CheckFixedArrayInitShape(r, &vd->type, fixedInit, leaf, vd->name, "local"))
        {
            return false;
        }

        /* The parser pre-fills elementType with the immediate element
           (`int[2]` for `int[2][2]`); codegen needs the LEAF (`int`),
           like struct-field inits (which parse with NULL and get the
           leaf here). */
        fixedInit->elementType = leaf;

        /* Short rows leave holes that zero-fill. */
        Vec flat;
        VecInit(&flat);

        for (ArrayLength z = 0; z < total; z++)
        {
            VecPush(&flat, NULL);
        }

        if (!PlaceFixedArrayInit(&flat, 0, &vd->type, fixedInit))
        {
            DiagErrorFmt(r->m_diag, vd->init->range, "too many initializers for fixed-size array local '%s' (%ld max)",
                         vd->name, total);
            return false;
        }

        void** oldItems = fixedInit->elements.items;
        fixedInit->elements = flat;
        free(oldItems);
    }
    return true;
}

static void WalkFixedArrayBracedInit(Resolver* r, VarDeclStmt* vd, ArrayInitExpr* fixedInit, const TypeName* leaf,
                                     StrMap* scope)
{
    /* Bare-brace leaves are struct inits of the leaf type. */
    for (size_t k = 0; k < fixedInit->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&fixedInit->elements, k);

        if (!elem)
        {
            continue;
        }

        if (elem->kind == NodeArrayInit || elem->kind == NodeStructInit)
        {
            VecSet(&fixedInit->elements, k, ApplyBracedStructTarget(r, elem, leaf));
        }
    }

    ResolveExpr(r, vd->init, scope);

    for (size_t k = 0; k < fixedInit->elements.count; k++)
    {
        Node* elem = (Node*)VecGet(&fixedInit->elements, k);

        if (!elem)
        {
            continue; // Hole: stays zero.
        }

        const TypeName* elemType = InferType(r, elem, scope);

        if (elemType && !IsAssignableType(r, leaf, elemType) && !IsStrLiteralForCString(r, leaf, elem))
        {
            DiagErrorFmt(r->m_diag, elem->range, "element of type '%s' cannot initialize '%s' element of local '%s'",
                         elemType->name, leaf->name, vd->name);
        }
    }
}

/* Stack-allocated fixed-size array locals (`float v[10] = { ... };`):
   inline `[N x T]` storage, braced initialization only. Params,
   returns and globals keep their bans (checked after all functions). */
static void WalkFixedArrayLocal(Resolver* r, VarDeclStmt* vd, StrMap* scope)
{
    ArrayLength total = 0;
    const TypeName* leaf = NULL;

    if (CheckFixedArrayLocal(r, vd, &leaf, &total))
    {
        ArrayInitExpr* fixedInit = (ArrayInitExpr*)vd->init;

        if (fixedInit->repeatCount)
        {
            WalkFixedArrayRepeatInit(r, vd, fixedInit, leaf, total, scope);
        }
        else if (FlattenFixedArrayInit(r, vd, fixedInit, leaf, total))
        {
            WalkFixedArrayBracedInit(r, vd, fixedInit, leaf, scope);
        }
    }

    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
}

static void CheckNonFixedLocal(Resolver* r, VarDeclStmt* vd)
{
    // Owning locals need an init (arrays/optionals exempt; a bare owning
    // struct local is rejected below with a sharper message).
    if (AliasIsOwning(r, &vd->type) && !TypeNameIsDynamicArray(&vd->type) && !TypeNameIsOptional(&vd->type)
        && !TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name) && !vd->init)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "box variable '%s' must be initialized", vd->name);
    }

    if (vd->type.isConst && !vd->init)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "const variable '%s' must be initialized", vd->name);
    }

    if (TypeRegistryIsOwningStruct(&r->m_registry, vd->type.name))
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "instances of '%s' must be boxed (i.e, '^%s').", vd->type.name,
                     vd->type.name);
    }

    // Dynamic arrays of owning structs (`Rec[]`) are legal: the fat owns
    // the buffer and drops each inline element recursively, just like
    // `string[]`. Fixed-array locals never reach this check (they are
    // non-owning by construction), so nothing here sees one.
}

static void WalkVarDeclInit(Resolver* r, VarDeclStmt* vd, StrMap* scope)
{
    ResolveExpr(r, vd->init, scope);
    vd->init = vd->init;
    const TypeName* initType = InferType(r, vd->init, scope);

    bool ok = initType && (IsAssignableType(r, &vd->type, initType) || IsStrLiteralForCString(r, &vd->type, vd->init));

    if (initType && !ok)
    {
        DiagErrorFmt(r->m_diag, vd->base.range, "'%s' cannot be initialized by expression of type '%s'", vd->type.name,
                     initType->name);
    }
    else if (ok)
    {
        CheckLiteralFits(r->m_diag, vd->init, &vd->type);
        CheckLossyConversion(r->m_diag, vd->init, initType, &vd->type, vd->base.range);
    }

    // Plain `T` from `T?` unwraps.

    // Box init from a box source moves it; a `Rec` source (owning
    // struct value, e.g. an element of a `Rec[]`) moves too.
    const char* movedInitKey = initType && AliasIsOwning(r, &vd->type) && AliasIsOwningValue(r, initType)
                                   ? MovableBoxSourceKey(r, vd->init)
                                   : NULL;

    if (movedInitKey)
    {
        if (TypeNameIsOptional(initType))
        {
            MoveOptionalSource(r, movedInitKey, vd->base.range);
        }
        else
        {
            MarkMoved(r, movedInitKey, vd->base.range);
        }
    }
}

static void WalkVarDeclStmt(Resolver* r, VarDeclStmt* vd, StrMap* scope)
{
    if (vd->isRef)
    {
        ResolveRefLocal(r, vd, scope);
        return;
    }

    /* A plain local shadows any earlier `ref` local of the same name. */
    if (StrMapGet(&r->m_refLocals, vd->name))
    {
        StrMapPut(&r->m_refLocals, vd->name, NULL);
        StrMapPut(&r->m_refBoxParams, vd->name, NULL);
    }

    if (!CheckLocalTypeKnown(r, vd))
    {
        return;
    }

    CheckLocalDeclTypes(r, vd);

    /* Resolve `[constName]` dimensions before the fixed-array checks below
       (idempotent: lengthName is cleared once resolved, so loop warmups
       re-resolving the same declaration are harmless). */
    SemaResolveConstDims(r, &vd->type);

    if (TypeTreeHasFixedArray(&vd->type))
    {
        WalkFixedArrayLocal(r, vd, scope);
        return;
    }

    CheckNonFixedLocal(r, vd);

    if (vd->init)
    {
        WalkVarDeclInit(r, vd, scope);
    }

    // Fresh binding: drop stale flow state for this name.

    ScopeDeclare(r, scope, vd->name, (void*)&vd->type, vd->base.range);
}

static void WalkExprStmt(Resolver* r, Node* n, StrMap* scope)
{
    ResolveExpr(r, ((ExprStmt*)n)->expr, scope);
    /* A call that never returns ends the path, as `return` does. */
    if (StmtIsNoReturnCall(n))
    {
        r->m_unreachable = true;
    }
}

static void WalkReturnStmt(Resolver* r, ReturnStmt* rs, StrMap* scope)
{
    if (r->m_deferWalkDepth > 0)
    {
        DiagErrorFmt(r->m_diag, rs->base.range, "'return' cannot leave a defer block");
    }

    if (rs->value)
    {
        if (r->m_currentReturnType && r->m_currentReturnType->primitiveType == PrimVoid)
        {
            DiagErrorFmt(r->m_diag, rs->base.range, "void function cannot return a value");
        }

        ResolveExpr(r, rs->value, scope);
        rs->value = rs->value;

        const TypeName* typeName = InferType(r, rs->value, scope);

        if (typeName && typeName->primitiveType == PrimVoid)
        {
            DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type 'void'");
        }

        // Check the returned value against the declared return type.
        if (r->m_currentReturnType && r->m_currentReturnType->primitiveType != PrimVoid && typeName
            && !IsAssignableType(r, r->m_currentReturnType, typeName)
            && !IsStrLiteralForCString(r, r->m_currentReturnType, rs->value))
        {
            DiagErrorFmt(r->m_diag, rs->base.range, "cannot return a value of type '%s' from a function returning '%s'",
                         typeName->name, r->m_currentReturnType->name);
        }

        if (r->m_currentReturnType && typeName)
        {
            CheckLossyConversion(r->m_diag, rs->value, typeName, r->m_currentReturnType, rs->base.range);
        }

        // Returning `T?` from `T` unwraps.

        // Moves only when the function returns an owning type (boxes,
        // strings, arrays, and owning structs — `return arr[i];` from a
        // `Rec`-returning function empties the element).
        const char* movedReturnKey
            = typeName && AliasIsOwningValue(r, typeName) ? MovableBoxSourceKey(r, rs->value) : NULL;

        if (movedReturnKey)
        {
            bool returnsSameBox = r->m_currentReturnType && strcmp(r->m_currentReturnType->name, typeName->name) == 0;

            if (returnsSameBox)
            {
                MarkMoved(r, movedReturnKey, rs->base.range);
            }
            else
            {
                /* Owning values without a box inner (string, T[]) have
                    nothing to "read out" into a differently-typed return,
                    so boxInner is NULL and no inner matching applies. The
                    type-mismatch diagnostic was already emitted above. */
                const TypeName* boxInner = TypeNameBoxInner(typeName);
                const char* boxInnerName = boxInner ? boxInner->name : NULL;

                bool innerIsOwning = boxInnerName && TypeRegistryIsOwningStruct(&r->m_registry, boxInnerName);
                bool innerMatchesReturn
                    = boxInnerName
                      && (r->m_currentReturnType
                          && (strcmp(r->m_currentReturnType->name, boxInnerName) == 0
                              || (IsNameNumeric(boxInnerName) && IsNameNumeric(r->m_currentReturnType->name))));

                if (innerIsOwning || !innerMatchesReturn)
                {
                    MarkMoved(r, movedReturnKey, rs->base.range);
                }
            }
        }
    }
    else if (r->m_currentReturnType && r->m_currentReturnType->primitiveType != PrimVoid)
    {
        DiagErrorFmt(r->m_diag, rs->base.range, "non-void function must return a value");
    }

    // Every pending defer runs after the value is computed.
    if (r->m_deferWalkDepth == 0 && !r->m_unreachable)
    {
        RunDefersFrom(r, 0, scope);
    }

    r->m_unreachable = true;
}

static void WalkIfStmt(Resolver* r, IfStmt* i, StrMap* scope)
{
    ResolveExpr(r, i->condition, scope);
    CheckBoolCondition(r, i->condition, scope);

    bool entryDead = r->m_unreachable;

    /* Each branch is its own lexical scope: locals declared in one
       branch are invisible in the other and after the join. */
    ScopePush(r);
    WalkStmt(r, i->thenBranch, scope);
    ScopePop(r, scope);

    bool thenDead = r->m_unreachable;
    r->m_unreachable = entryDead;

    if (i->elseBranch)
    {
        ScopePush(r);
        WalkStmt(r, i->elseBranch, scope);
        ScopePop(r, scope);
    }

    /* The join is unreachable only when both arms are. An else-less if falls through on its false edge. */
    r->m_unreachable = thenDead && r->m_unreachable;
}

static void WalkWhileStmt(Resolver* r, WhileStmt* w, StrMap* scope)
{
    /* The loop body is its own scope (covers non-block bodies too;
       block bodies nest a second scope harmlessly). */
    WalkLoop(r, w->condition, NULL, w->body, true, scope);
}

static void WalkForStmt(Resolver* r, ForStmt* fs, StrMap* scope)
{
    /* The whole loop (init, condition, update, body) is one scope so a
       `for (int i ...)` header variable is visible in the body but does
       not leak after the loop. */
    ScopePush(r);

    if (fs->init)
    {
        if (fs->init->kind == NodeVarDecl)
        {
            WalkStmt(r, fs->init, scope);
        }
        else
        {
            ResolveExpr(r, fs->init, scope);
        }
    }

    // The condition and update run on every iteration, not just once.
    WalkLoop(r, fs->condition, fs->update, fs->body, false, scope);

    ScopePop(r, scope);
}

static void WalkStmt(Resolver* r, Node* n, StrMap* scope)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeBlock:
        WalkBlock(r, (Block*)n, scope);
        return;

    case NodeVarDecl:
        WalkVarDeclStmt(r, (VarDeclStmt*)n, scope);
        return;

    case NodeExprStmt:
        WalkExprStmt(r, n, scope);
        return;

    case NodeDefer:
        // Runs (and is checked) at each exit of the enclosing scope.
        QueueDefer(r, (DeferStmt*)n, scope);
        return;

    case NodeBreak:
        WalkLoopJump(r, scope, true, n->range);
        return;

    case NodeContinue:
        WalkLoopJump(r, scope, false, n->range);
        return;

    case NodeReturn:
        WalkReturnStmt(r, (ReturnStmt*)n, scope);
        return;

    case NodeIf:
        WalkIfStmt(r, (IfStmt*)n, scope);
        return;

    case NodeWhile:
        WalkWhileStmt(r, (WhileStmt*)n, scope);
        return;

    case NodeFor:
        WalkForStmt(r, (ForStmt*)n, scope);
        return;

    default:
        return;
    }
}

void WalkBlock(Resolver* r, Block* b, StrMap* scope)
{
    ScopePush(r);

    for (size_t i = 0; i < b->statements.count; i++)
    {
        WalkStmt(r, (Node*)VecGet(&b->statements, i), scope);
    }

    ScopePop(r, scope);
}

/* ---- Missing-return flow analysis ---- */
static bool ExprIsConstantTrue(const Node* n)
{
    if (!n)
    {
        return false;
    }
    switch (n->kind)
    {
    case NodeIntLiteral:
        return ((const IntLiteral*)n)->value != 0;
    case NodeBoolLiteral:
        return ((const BoolLiteral*)n)->value;
    case NodeUnary:
        if (((const UnaryExpr*)n)->op == UnNot)
        {
            return ExprIsConstantFalse(((const UnaryExpr*)n)->operand);
        }
        return false;
    default:
        return false;
    }
}

static bool ExprIsConstantFalse(const Node* n)
{
    if (!n)
    {
        return false;
    }
    switch (n->kind)
    {
    case NodeIntLiteral:
        return ((const IntLiteral*)n)->value == 0;
    case NodeBoolLiteral:
        return !((const BoolLiteral*)n)->value;
    case NodeUnary:
        if (((const UnaryExpr*)n)->op == UnNot)
        {
            return ExprIsConstantTrue(((const UnaryExpr*)n)->operand);
        }
        return false;
    default:
        return false;
    }
}

/* True if a `break` targeting the loop at `targetDepth` can be reached on some
   path through `n`, where `depth` is the current loop-nesting depth. A `break`
   inside a nested loop targets that loop, not ours. */
static bool BodyCanBreak(const Node* n, int depth, int targetDepth)
{
    if (!n)
    {
        return false;
    }

    switch (n->kind)
    {
    case NodeBreak:
        return depth == targetDepth;
    case NodeBlock:
    {
        const Block* b = (const Block*)n;
        for (size_t i = 0; i < b->statements.count; i++)
        {
            if (BodyCanBreak((const Node*)VecGet(&b->statements, i), depth, targetDepth))
            {
                return true;
            }
        }
        return false;
    }
    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        return BodyCanBreak(s->thenBranch, depth, targetDepth) || BodyCanBreak(s->elseBranch, depth, targetDepth);
    }
    case NodeWhile:
        return BodyCanBreak(((const WhileStmt*)n)->body, depth + 1, targetDepth);
    case NodeFor:
        return BodyCanBreak(((const ForStmt*)n)->body, depth + 1, targetDepth);
    default:
        return false;
    }
}

/* True if control can reach the statement after `n` (fall off its end) on some
   path. `depth` is the current loop-nesting depth. */
bool StmtFallsThrough(const Node* n, int depth)
{
    if (!n)
    {
        return true;
    }

    if (StmtIsNoReturnCall(n))
    {
        return false;
    }

    switch (n->kind)
    {
    case NodeReturn:
    case NodeBreak:
    case NodeContinue:
        return false;

    case NodeBlock:
    {
        const Block* b = (const Block*)n;
        for (size_t i = 0; i < b->statements.count; i++)
        {
            if (!StmtFallsThrough((const Node*)VecGet(&b->statements, i), depth))
            {
                return false; /* terminating statement hit; rest is dead */
            }
        }
        return true;
    }

    case NodeIf:
    {
        const IfStmt* s = (const IfStmt*)n;
        if (ExprIsConstantTrue(s->condition))
        {
            return StmtFallsThrough(s->thenBranch, depth);
        }
        if (ExprIsConstantFalse(s->condition))
        {
            return StmtFallsThrough(s->elseBranch, depth);
        }
        if (!s->elseBranch)
        {
            return true; /* false branch always falls through */
        }
        return StmtFallsThrough(s->thenBranch, depth) || StmtFallsThrough(s->elseBranch, depth);
    }

    case NodeWhile:
    {
        const WhileStmt* s = (const WhileStmt*)n;
        if (ExprIsConstantTrue(s->condition))
        {
            /* Infinite unless the body breaks out. */
            return BodyCanBreak(s->body, depth + 1, depth + 1);
        }
        /* May run zero times -> control reaches after the loop. */
        return true;
    }

    case NodeFor:
    {
        const ForStmt* s = (const ForStmt*)n;
        if (!s->condition || ExprIsConstantTrue(s->condition))
        {
            /* for(;;) or for(;true;) : infinite unless body breaks. */
            return BodyCanBreak(s->body, depth + 1, depth + 1);
        }
        return true;
    }

    default:
        return true;
    }
}
