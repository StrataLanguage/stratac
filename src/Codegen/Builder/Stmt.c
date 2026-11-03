/* Builder: Statements, block scopes and defers. */
#include "Codegen/Builder/BuilderInternal.h"

/* Forward declarations for functions used before their definition in this file. */
static void EmitStmtInner(Builder* b, Node* n);

/* A `defer` scope: deferred statements queued for the enclosing block's exit. */
void PushScope(Builder* b)
{
    BlockScope* bs = (BlockScope*)ArenaAlloc(b->arena, sizeof(BlockScope));
    VecInit(&bs->defers);
    VecPush(&b->scopes, bs);
}

void PopScope(Builder* b)
{
    VecPop(&b->scopes);
}

BlockScope* TopScope(Builder* b)
{
    if (b->scopes.count == 0)
    {
        return NULL;
    }

    return (BlockScope*)VecGet(&b->scopes, b->scopes.count - 1);
}

/* ---- Lexical symbol scopes ----
   `symbols` is flat, so each lexical block records the locals it declares;
   SymPop removes exactly those again. Sema already rejects redefinitions, so
   codegen silently overwrites (invalid programs never reach emission with
   diagnostics clean, and valid ones never collide). */
void SymPush(Builder* b)
{
    Vec* frame = (Vec*)ArenaAlloc(b->arena, sizeof(Vec));
    VecInit(frame);
    VecPush(&b->symDecls, frame);
}

void SymPop(Builder* b)
{
    if (b->symDecls.count == 0)
    {
        return;
    }

    Vec* frame = (Vec*)VecPop(&b->symDecls);

    for (size_t i = 0; i < frame->count; i++)
    {
        StrMapPut(&b->symbols, (const char*)VecGet(frame, i), NULL);
    }
}

void SymDeclare(Builder* b, const char* name, Value* sym)
{
    StrMapPut(&b->symbols, name, sym);

    if (b->symDecls.count > 0)
    {
        Vec* top = (Vec*)VecGet(&b->symDecls, b->symDecls.count - 1);
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
        if (b->terminated)
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
    for (size_t s = b->scopes.count; s-- > fromIndex;)
    {
        RunDefers(b, (BlockScope*)VecGet(&b->scopes, s));
    }
}

/* Emits one statement, then drops the owning temporaries its expressions
   borrowed (see BorrowTemp): a full statement is the temporaries' lifetime. */
void EmitStmt(Builder* b, Node* n)
{
    StackMark tempMark = b->temps.count;

    EmitStmtInner(b, n);
    FlushTemps(b, tempMark);
}

static void EmitReturnStmt(Builder* b, ReturnStmt* r)
{
    Value v = {0};

    if (r->value)
    {
        Value raw = EmitExpr(b, r->value);

        if (TypeDescIsBox(&b->curRet) && !TypeDescIsBox(&raw.typeDesc))
        {
            /* Returns ^T but expr is a plain T: box it, like a local. */
            TypeDesc innerTd = Resolve(b, b->curRet.boxInner);
            LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "heap");
            LLVMBuildStore(b->builder, Coerce(b, raw, innerTd).value, heap);
            v = ValueMake(heap, b->curRet);
        }
        else
        {
            v = UnboxIfBox(b, Coerce(b, raw, b->curRet), b->curRet);

            if (TypeDescIsString(&v.typeDesc))
            {
                /* Returning a string: construct the owned value
                   (heap-copy literal, move source). */
                LLVMValueRef owned = EmitOwnedValue(b, v, r->value, StringTypeName(b));
                v = ValueMake(owned, b->curRet);
            }

            Node* movedReturnNode = (TypeDescIsBox(&v.typeDesc) || TypeDescHasArrayShape(&v.typeDesc)
                                     || (v.typeDesc.structTypeName
                                         && TypeRegistryIsOwningStruct(&b->registry, v.typeDesc.structTypeName)))
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
        LLVMBuildRet(b->builder, ExtendToRetWidth(b, v.value, b->curRet, b->curRetAbi));
    }
    else
    {
        LLVMBuildRetVoid(b->builder);
    }

    b->terminated = true;
}

static void EmitExprStmt(Builder* b, Node* n)
{
    ExprStmt* e = (ExprStmt*)n;
    EmitDiscardExpr(b, e->expr);

    /* Past a `@noreturn` call nothing runs: no drops, no defers, no fall-through return. */
    if (StmtIsNoReturnCall(n))
    {
        LLVMBuildUnreachable(b->builder);
        b->terminated = true;
    }
}

static void EmitRefVarDecl(Builder* b, VarDeclStmt* varDecl)
{
    TypeDesc refTd = Resolve(b, &varDecl->type);
    LValue lv = EmitLValue(b, varDecl->init);
    LLVMValueRef addr = lv.valid ? lv.ptr : LLVMConstNull(b->ptrTy);

    if (lv.valid && varDecl->refThroughBox)
    {
        addr = LLVMBuildLoad2(b->builder, b->ptrTy, lv.ptr, "ref.box");
    }

    Value* sym = (Value*)ArenaAlloc(b->arena, sizeof(Value));
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
        LLVMBuildStore(b->builder, value.value, slot);
    }
    else
    {
        /* A bare `cstring` defaults to the static empty buffer (never
           NULL), mirroring the extern-boundary invariant. */
        LLVMBuildStore(b->builder,
                       TypeDescIsCString(&typeDesc) ? EmptyNulString(b) : DefaultValueOf(b, &varDecl->type, typeDesc),
                       slot);
    }

    Value* sym = (Value*)ArenaAlloc(b->arena, sizeof(Value));
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

    if (TypeDescIsFixedArray(&typeDesc))
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
    StackMark mark = b->owningLocals.count;

    PushScope(b);
    SymPush(b);

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        EmitStmt(b, (Node*)VecGet(&blk->statements, i));

        if (b->terminated)
        {
            break;
        }
    }

    if (!b->terminated)
    {
        RunDefers(b, TopScope(b));
    }

    SymPop(b);
    PopScope(b);

    if (!b->terminated)
    {
        EmitDrops(b, mark);
    }

    b->owningLocals.count = mark;
}

static void EmitIfStmt(Builder* b, IfStmt* i)
{
    LLVMValueRef cond = EmitConditionI1(b, i->condition);

    LLVMBasicBlockRef thenBB = NewBb(b, "if.then");
    LLVMBasicBlockRef endBB = NewBb(b, "if.end");
    LLVMBasicBlockRef elseBB = i->elseBranch ? NewBb(b, "if.else") : endBB;

    LLVMBuildCondBr(b->builder, cond, thenBB, elseBB);

    b->terminated = true;

    PositionAtEnd(b, thenBB);
    PushScope(b);
    SymPush(b);
    EmitStmt(b, i->thenBranch);
    if (!b->terminated)
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
        if (!b->terminated)
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

    LLVMBuildBr(b->builder, condBB);

    b->terminated = true;

    PositionAtEnd(b, condBB);

    LLVMValueRef cond = EmitConditionI1(b, w->condition);

    LLVMBuildCondBr(b->builder, cond, bodyBB, endBB);

    b->terminated = true;

    PositionAtEnd(b, bodyBB);

    Loop* loop = (Loop*)ArenaAlloc(b->arena, sizeof(Loop));
    loop->cont = condBB;
    loop->end = endBB;
    loop->bodyMark = b->owningLocals.count;
    loop->scopeDepth = b->scopes.count;
    VecPush(&b->loops, loop);

    PushScope(b);
    SymPush(b);
    EmitStmt(b, w->body);
    bool term = b->terminated;
    if (!term)
    {
        RunDefers(b, TopScope(b));
    }
    SymPop(b);
    PopScope(b);
    VecPop(&b->loops);

    if (!term)
    {
        Br(b, condBB);
    }

    PositionAtEnd(b, endBB);
}

static void EmitForStmt(Builder* b, ForStmt* fs)
{
    StackMark headerMark = b->owningLocals.count;

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
        LLVMBuildCondBr(b->builder, EmitConditionI1(b, fs->condition), bodyBB, endBB);
    }
    else
    {
        LLVMBuildBr(b->builder, bodyBB);
    }

    b->terminated = true;

    PositionAtEnd(b, bodyBB);

    Loop* loop = (Loop*)ArenaAlloc(b->arena, sizeof(Loop));
    loop->cont = updBB;
    loop->end = endBB;
    loop->bodyMark = b->owningLocals.count;
    loop->scopeDepth = b->scopes.count;
    VecPush(&b->loops, loop);

    PushScope(b);
    SymPush(b);
    EmitStmt(b, fs->body);
    bool term = b->terminated;
    if (!term)
    {
        RunDefers(b, TopScope(b));
    }
    SymPop(b);
    PopScope(b);
    VecPop(&b->loops);

    if (!term)
    {
        Br(b, updBB);
    }

    PositionAtEnd(b, updBB);

    if (fs->update)
    {
        StackMark tempMark = b->temps.count;
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
    b->owningLocals.count = headerMark;

    SymPop(b);
}

static void EmitBreakStmt(Builder* b, Node* n)
{
    if (b->loops.count > 0)
    {
        Loop* loop = (Loop*)VecGet(&b->loops, b->loops.count - 1);
        RunDefersFrom(b, loop->scopeDepth);
        /* Drops only on this edge: the locals stay registered for the
           code that follows (an unbraced `if (c) break;` falls through
           to the rest of the body, which must still drop them). */
        EmitDrops(b, loop->bodyMark);
        Br(b, loop->end);
    }
    else
    {
        DiagError(b->diag, n->range, "break statement not inside a loop");
    }
}

static void EmitContinueStmt(Builder* b, Node* n)
{
    if (b->loops.count > 0)
    {
        Loop* loop = (Loop*)VecGet(&b->loops, b->loops.count - 1);
        RunDefersFrom(b, loop->scopeDepth);
        /* Edge-only drops, as for break. */
        EmitDrops(b, loop->bodyMark);
        Br(b, loop->cont);
    }
    else
    {
        DiagError(b->diag, n->range, "continue statement not inside a loop");
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
