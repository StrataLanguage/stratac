/* Builder: Statements, block scopes and defers. */
#include "Codegen/Builder/BuilderInternal.h"

/* Forward declarations for functions used before their definition in this file. */
static void EmitStmtInner(Builder* b, Node* n);

/* A `defer` scope: deferred statements queued for the enclosing block's exit. */
void PushScope(Builder* b)
{
    BlockScope* bs = (BlockScope*)arena_alloc(b->m_arena, sizeof(BlockScope));
    VecInit(&bs->defers);
    VecPush(&b->m_scopes, bs);
}

void PopScope(Builder* b)
{
    VecPop(&b->m_scopes);
}

BlockScope* TopScope(Builder* b)
{
    if (b->m_scopes.count == 0)
    {
        return NULL;
    }

    return (BlockScope*)VecGet(&b->m_scopes, b->m_scopes.count - 1);
}

/* ---- Lexical symbol scopes ----
   `m_symbols` is flat, so each lexical block records the locals it declares;
   SymPop removes exactly those again. Sema already rejects redefinitions, so
   codegen silently overwrites (invalid programs never reach emission with
   diagnostics clean, and valid ones never collide). */
void SymPush(Builder* b)
{
    Vec* frame = (Vec*)arena_alloc(b->m_arena, sizeof(Vec));
    VecInit(frame);
    VecPush(&b->m_symDecls, frame);
}

void SymPop(Builder* b)
{
    if (b->m_symDecls.count == 0)
    {
        return;
    }

    Vec* frame = (Vec*)VecPop(&b->m_symDecls);

    for (size_t i = 0; i < frame->count; i++)
    {
        StrMapPut(&b->m_symbols, (const char*)VecGet(frame, i), NULL);
    }
}

void SymDeclare(Builder* b, const char* name, Value* sym)
{
    StrMapPut(&b->m_symbols, name, sym);

    if (b->m_symDecls.count > 0)
    {
        Vec* top = (Vec*)VecGet(&b->m_symDecls, b->m_symDecls.count - 1);
        VecPush(top, (void*)name);
    }
}

/* Run one scope's deferred statements, LIFO (last `defer` first), Zig-style. */
void RunDefers(Builder* b, BlockScope* scope)
{
    if (!scope)
    {
        return;
    }

    for (size_t i = scope->defers.count; i-- > 0;)
    {
        if (b->m_terminated)
        {
            break;
        }

        EmitStmt(b, (Node*)VecGet(&scope->defers, i));
    }
}

/* Run deferred statements of every active scope in [fromIndex, count), innermost
   scope first (Zig exits inner blocks before outer ones), each scope LIFO. */
static void RunDefersFrom(Builder* b, StackMark fromIndex)
{
    for (size_t s = b->m_scopes.count; s-- > fromIndex;)
    {
        RunDefers(b, (BlockScope*)VecGet(&b->m_scopes, s));
    }
}

/* Emits one statement, then drops the owning temporaries its expressions
   borrowed (see BorrowTemp): a full statement is the temporaries' lifetime. */
void EmitStmt(Builder* b, Node* n)
{
    StackMark tempMark = b->m_temps.count;

    EmitStmtInner(b, n);
    FlushTemps(b, tempMark);
}

static void EmitReturnStmt(Builder* b, ReturnStmt* r)
{
    Value v = {0};

    if (r->value)
    {
        Value raw = EmitExpr(b, r->value);

        if (TypeDescIsBox(&b->m_curRet) && !TypeDescIsBox(&raw.typeDesc))
        {
            /* Returns ^T but expr is a plain T: box it, like a local. */
            TypeDesc innerTd = Resolve(b, b->m_curRet.boxInner);
            LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "heap");
            LLVMBuildStore(b->m_builder, Coerce(b, raw, innerTd).value, heap);
            v = ValueMake(heap, b->m_curRet);
        }
        else
        {
            v = UnboxIfBox(b, Coerce(b, raw, b->m_curRet), b->m_curRet);

            if (v.typeDesc.isString)
            {
                /* Returning a string: construct the owned value
                   (heap-copy literal, move source). */
                LLVMValueRef owned = EmitOwnedValue(b, v, r->value, StringTypeName(b));
                v = ValueMake(owned, b->m_curRet);
            }

            Node* movedReturnNode = (TypeDescIsBox(&v.typeDesc) || TypeDescHasArrayShape(&v.typeDesc)
                                     || (v.typeDesc.structTypeName
                                         && TypeRegistryIsOwningStruct(&b->m_registry, v.typeDesc.structTypeName)))
                                        ? MovedSource(r->value)
                                        : NULL;

            if (movedReturnNode)
            {
                NullMovedSource(b, r->value);
            }
        }
    }

    /* The return value is computed: temporaries it borrowed die now,
       before the defers run (a defer is a statement of its own). */
    FlushTemps(b, 0);

    RunDefersFrom(b, 0);
    EmitDrops(b, 0);

    if (r->value)
    {
        LLVMBuildRet(b->m_builder, ExtendToRetWidth(b, v.value, b->m_curRet, b->m_curRetAbi));
    }
    else
    {
        LLVMBuildRetVoid(b->m_builder);
    }

    b->m_terminated = true;
}

static void EmitExprStmt(Builder* b, Node* n)
{
    ExprStmt* e = (ExprStmt*)n;
    EmitDiscardExpr(b, e->expr);

    /* Past a `@noreturn` call nothing runs: no drops, no defers, no fall-through return. */
    if (StmtIsNoReturnCall(n))
    {
        LLVMBuildUnreachable(b->m_builder);
        b->m_terminated = true;
    }
}

static void EmitRefVarDecl(Builder* b, VarDeclStmt* varDecl)
{
    TypeDesc refTd = Resolve(b, &varDecl->type);
    LValue lv = EmitLValue(b, varDecl->init);
    LLVMValueRef addr = lv.valid ? lv.ptr : LLVMConstNull(b->m_ptrTy);

    if (lv.valid && varDecl->refThroughBox)
    {
        addr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "ref.box");
    }

    Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
    sym->value = addr;
    sym->typeDesc = refTd;
    SymDeclare(b, varDecl->name, sym);
}

static void EmitScalarVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc)
{
    LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "v");

    if (varDecl->init)
    {
        Value value = UnboxIfBox(b, Coerce(b, EmitExpr(b, varDecl->init), typeDesc), typeDesc);
        LLVMBuildStore(b->m_builder, value.value, slot);
    }
    else
    {
        /* A bare `cstring` defaults to the static empty buffer (never
           NULL), mirroring the extern-boundary invariant. */
        LLVMBuildStore(b->m_builder,
                       typeDesc.isCString ? EmptyNulString(b) : DefaultValueOf(b, &varDecl->type, typeDesc), slot);
    }

    Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
    sym->value = slot;
    sym->typeDesc = typeDesc;

    SymDeclare(b, varDecl->name, sym);
}

static void EmitVarDecl(Builder* b, VarDeclStmt* varDecl)
{
    /* `ref T x = path;`: the symbol is the address of the storage itself, never dropped here. */
    if (varDecl->isRef)
    {
        EmitRefVarDecl(b, varDecl);
        return;
    }

    TypeDesc typeDesc = Resolve(b, &varDecl->type);

    if (typeDesc.isFixedArray)
    {
        EmitFixedArrayVarDecl(b, varDecl, typeDesc);
        return;
    }

    if (TypeDescHasArrayShape(&typeDesc))
    {
        EmitArrayVarDecl(b, varDecl, typeDesc);
        return;
    }

    if (TypeDescIsBox(&typeDesc))
    {
        EmitBoxVarDecl(b, varDecl, typeDesc);
        return;
    }

    EmitScalarVarDecl(b, varDecl, typeDesc);
}

static void EmitDeferStmt(Builder* b, DeferStmt* d)
{
    BlockScope* scope = TopScope(b);

    if (scope)
    {
        VecPush(&scope->defers, d->stmt);
    }
}

static void EmitBlockStmt(Builder* b, Block* blk)
{
    StackMark mark = b->m_owningLocals.count;

    PushScope(b);
    SymPush(b);

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        EmitStmt(b, (Node*)VecGet(&blk->statements, i));

        if (b->m_terminated)
        {
            break;
        }
    }

    if (!b->m_terminated)
    {
        RunDefers(b, TopScope(b));
    }

    SymPop(b);
    PopScope(b);

    if (!b->m_terminated)
    {
        EmitDrops(b, mark);
    }

    b->m_owningLocals.count = mark;
}

static void EmitIfStmt(Builder* b, IfStmt* i)
{
    LLVMValueRef cond = EmitConditionI1(b, i->condition);

    LLVMBasicBlockRef thenBB = NewBb(b, "if.then");
    LLVMBasicBlockRef endBB = NewBb(b, "if.end");
    LLVMBasicBlockRef elseBB = i->elseBranch ? NewBb(b, "if.else") : endBB;

    LLVMBuildCondBr(b->m_builder, cond, thenBB, elseBB);

    b->m_terminated = true;

    PositionAtEnd(b, thenBB);
    PushScope(b);
    SymPush(b);
    EmitStmt(b, i->thenBranch);
    if (!b->m_terminated)
    {
        RunDefers(b, TopScope(b));
    }
    SymPop(b);
    PopScope(b);
    Br(b, endBB);

    if (i->elseBranch)
    {
        PositionAtEnd(b, elseBB);
        PushScope(b);
        SymPush(b);
        EmitStmt(b, i->elseBranch);
        if (!b->m_terminated)
        {
            RunDefers(b, TopScope(b));
        }
        SymPop(b);
        PopScope(b);
        Br(b, endBB);
    }

    PositionAtEnd(b, endBB);
}

static void EmitWhileStmt(Builder* b, WhileStmt* w)
{
    LLVMBasicBlockRef condBB = NewBb(b, "while.cond");
    LLVMBasicBlockRef bodyBB = NewBb(b, "while.body");
    LLVMBasicBlockRef endBB = NewBb(b, "while.end");

    LLVMBuildBr(b->m_builder, condBB);

    b->m_terminated = true;

    PositionAtEnd(b, condBB);

    LLVMValueRef cond = EmitConditionI1(b, w->condition);

    LLVMBuildCondBr(b->m_builder, cond, bodyBB, endBB);

    b->m_terminated = true;

    PositionAtEnd(b, bodyBB);

    Loop* loop = (Loop*)arena_alloc(b->m_arena, sizeof(Loop));
    loop->cont = condBB;
    loop->end = endBB;
    loop->bodyMark = b->m_owningLocals.count;
    loop->scopeDepth = b->m_scopes.count;
    VecPush(&b->m_loops, loop);

    PushScope(b);
    SymPush(b);
    EmitStmt(b, w->body);
    bool term = b->m_terminated;
    if (!term)
    {
        RunDefers(b, TopScope(b));
    }
    SymPop(b);
    PopScope(b);
    VecPop(&b->m_loops);

    if (!term)
    {
        Br(b, condBB);
    }

    PositionAtEnd(b, endBB);
}

static void EmitForStmt(Builder* b, ForStmt* fs)
{
    StackMark headerMark = b->m_owningLocals.count;

    SymPush(b);

    if (fs->init)
    {
        EmitStmt(b, fs->init);
    }

    LLVMBasicBlockRef condBB = NewBb(b, "for.cond");
    LLVMBasicBlockRef bodyBB = NewBb(b, "for.body");
    LLVMBasicBlockRef updBB = NewBb(b, "for.update");
    LLVMBasicBlockRef endBB = NewBb(b, "for.end");

    Br(b, condBB);

    PositionAtEnd(b, condBB);

    if (fs->condition)
    {
        LLVMBuildCondBr(b->m_builder, EmitConditionI1(b, fs->condition), bodyBB, endBB);
    }
    else
    {
        LLVMBuildBr(b->m_builder, bodyBB);
    }

    b->m_terminated = true;

    PositionAtEnd(b, bodyBB);

    Loop* loop = (Loop*)arena_alloc(b->m_arena, sizeof(Loop));
    loop->cont = updBB;
    loop->end = endBB;
    loop->bodyMark = b->m_owningLocals.count;
    loop->scopeDepth = b->m_scopes.count;
    VecPush(&b->m_loops, loop);

    PushScope(b);
    SymPush(b);
    EmitStmt(b, fs->body);
    bool term = b->m_terminated;
    if (!term)
    {
        RunDefers(b, TopScope(b));
    }
    SymPop(b);
    PopScope(b);
    VecPop(&b->m_loops);

    if (!term)
    {
        Br(b, updBB);
    }

    PositionAtEnd(b, updBB);

    if (fs->update)
    {
        StackMark tempMark = b->m_temps.count;
        EmitDiscardExpr(b, fs->update);
        FlushTemps(b, tempMark);
    }

    /* for.update is always reachable via `continue` (or is simply dead
       when the body never falls through); either way it needs a
       terminator, so don't key this on whether the body terminated. */
    Br(b, condBB);

    PositionAtEnd(b, endBB);

    /* The init's owning locals (`for (^int k = ...; ...)`) live for the
       whole loop and die here, on normal exit and on `break` alike (break
       only drops the body's locals, then branches here). */
    EmitDrops(b, headerMark);
    b->m_owningLocals.count = headerMark;

    SymPop(b);
}

static void EmitBreakStmt(Builder* b, Node* n)
{
    if (b->m_loops.count > 0)
    {
        Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
        RunDefersFrom(b, loop->scopeDepth);
        /* Drops only on this edge: the locals stay registered for the
           code that follows (an unbraced `if (c) break;` falls through
           to the rest of the body, which must still drop them). */
        EmitDrops(b, loop->bodyMark);
        Br(b, loop->end);
    }
    else
    {
        DiagError(b->m_diag, n->range, "break statement not inside a loop");
    }
}

static void EmitContinueStmt(Builder* b, Node* n)
{
    if (b->m_loops.count > 0)
    {
        Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
        RunDefersFrom(b, loop->scopeDepth);
        /* Edge-only drops, as for break. */
        EmitDrops(b, loop->bodyMark);
        Br(b, loop->cont);
    }
    else
    {
        DiagError(b->m_diag, n->range, "continue statement not inside a loop");
    }
}

static void EmitStmtInner(Builder* b, Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeReturn:
        EmitReturnStmt(b, (ReturnStmt*)n);
        break;

    case NodeExprStmt:
        EmitExprStmt(b, n);
        break;

    case NodeVarDecl:
        EmitVarDecl(b, (VarDeclStmt*)n);
        break;

    case NodeDefer:
        EmitDeferStmt(b, (DeferStmt*)n);
        break;

    case NodeBlock:
        EmitBlockStmt(b, (Block*)n);
        break;

    case NodeIf:
        EmitIfStmt(b, (IfStmt*)n);
        break;

    case NodeWhile:
        EmitWhileStmt(b, (WhileStmt*)n);
        break;

    case NodeFor:
        EmitForStmt(b, (ForStmt*)n);
        break;

    case NodeBreak:
        EmitBreakStmt(b, n);
        break;

    case NodeContinue:
        EmitContinueStmt(b, n);
        break;

    default:
    {
        (void)EmitExpr(b, n);
        return;
    }
    }
}
