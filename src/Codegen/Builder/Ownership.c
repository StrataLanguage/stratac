/* Builder: Ownership: drops, temporaries, scopes of owned values, dummies, copies. */
#include "Codegen/Builder/BuilderInternal.h"

/* Forward declarations for functions used before their definition in this file. */
static void EmitDummyStore(Builder* b, LLVMValueRef slot, TypeDesc td, Vec* chain);

LLVMValueRef SharedDummyAddr(Builder* b, TypeDesc td)
{
    char* gName = arena_format(b->m_arena, ".oob.zero.%d", b->m_strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->m_mod, td.type, gName);
    LLVMSetInitializer(g, LLVMConstNull(td.type));
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMValueRef idx[1] = {LLVMConstInt(I32Ty(b), 0, 0)};
    return LLVMConstGEP2(td.type, g, idx, 1);
}

static bool ChainContains(const Vec* chain, const char* name)
{
    for (size_t i = 0; i < chain->count; i++)
    {
        if (strcmp((const char*)VecGet(chain, i), name) == 0)
        {
            return true;
        }
    }

    return false;
}

/* True when an out-of-bounds element needs private, re-initialized scratch
   instead of the shared zeroed global: the slot can be freed through, moved
   out of, or re-bound, so each OOB event must install a fresh dummy. */
bool ElemNeedsScratchDummy(Builder* b, TypeDesc td)
{
    if (TypeDescHasArrayShape(&td) || TypeDescIsBox(&td))
    {
        return true;
    }

    return td.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, td.structTypeName);
}

/* Stores a fresh dummy of `td` into `slot` (assumed already dropped):
   string -> heap "", ^T -> new T, owning struct -> field-wise construct,
   T[] -> {NULL,0}. `chain` guards compile-time recursion: a field whose
   struct type is already under construction gets a zero value. */
/* Iterates the fields of an owning struct, dummy-storing owning fields
   recursively and zeroing the rest, into `base`. */
static void EmitDummyStoreStructFields(Builder* b, LLVMValueRef base, const StructType* st, LLVMTypeRef structTy,
                                       Vec* chain, const char* structName)
{
    VecPush(chain, (void*)structName);

    for (size_t j = 0; j < st->fields.count; j++)
    {
        FieldDecl* f = (FieldDecl*)VecGet(&st->fields, j);
        TypeDesc fieldTd = Resolve(b, &f->type);
        LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)j))};
        LLVMValueRef fieldAddr = LLVMBuildGEP2(b->m_builder, structTy, base, idxs, 2, "df");

        if (BuilderIsOwningType(b, &f->type) || TypeRegistryIsOwningStruct(&b->m_registry, f->type.name))
        {
            EmitDummyStore(b, fieldAddr, fieldTd, chain);
        }
        else
        {
            LLVMBuildStore(b->m_builder, LLVMConstNull(fieldTd.type), fieldAddr);
        }
    }

    VecPop(chain);
}

static void EmitDummyStore(Builder* b, LLVMValueRef slot, TypeDesc td, Vec* chain)
{
    if (!td.type)
    {
        return;
    }

    if (TypeDescHasArrayShape(&td))
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), slot);
        return;
    }

    if (TypeDescIsBox(&td) && !td.boxInner)
    {
        StrataStrdupFn(b);
        LLVMValueRef args[1] = {EmptyStrConstant(b)};
        LLVMValueRef dup = LLVMBuildCall2(b->m_builder, b->m_strdupFnType, b->m_strdupFn, args, 1, "dstr");
        LLVMBuildStore(b->m_builder, dup, slot);
        return;
    }

    if (TypeDescIsBox(&td) && td.boxInner)
    {
        TypeDesc innerTd = Resolve(b, td.boxInner);
        const StructType* st = TypeRegistryFind(&b->m_registry, td.boxInner->name);
        LLVMTypeRef structTy = st ? (LLVMTypeRef)StrMapGet(&b->m_structTypes, td.boxInner->name) : NULL;

        if (st && structTy && TypeRegistryIsOwningStruct(&b->m_registry, td.boxInner->name))
        {
            if (ChainContains(chain, td.boxInner->name))
            {
                LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), slot);
                return;
            }

            LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "dbox");

            EmitDummyStoreStructFields(b, heap, st, structTy, chain, td.boxInner->name);
            LLVMBuildStore(b->m_builder, heap, slot);
            return;
        }

        /* Inner is a scalar, string, another box, or an array: allocate and
           initialize the single inner value. */
        LLVMValueRef heap = AllocBoxCell(b, SizeOfConst(b, innerTd.type), "dbox");

        if (TypeDescHasArrayShape(&innerTd) || TypeDescIsBox(&innerTd))
        {
            EmitDummyStore(b, heap, innerTd, chain);
        }
        else
        {
            LLVMBuildStore(b->m_builder, LLVMConstNull(innerTd.type), heap);
        }

        LLVMBuildStore(b->m_builder, heap, slot);
        return;
    }

    if (td.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, td.structTypeName))
    {
        if (ChainContains(chain, td.structTypeName))
        {
            LLVMBuildStore(b->m_builder, LLVMConstNull(td.type), slot);
            return;
        }

        const StructType* st = TypeRegistryFind(&b->m_registry, td.structTypeName);
        LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, td.structTypeName);

        if (!st || !structTy)
        {
            LLVMBuildStore(b->m_builder, LLVMConstNull(td.type), slot);
            return;
        }

        EmitDummyStoreStructFields(b, slot, st, structTy, chain, td.structTypeName);
        return;
    }

    LLVMBuildStore(b->m_builder, LLVMConstNull(td.type), slot);
}

void EmitDummyInit(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    Vec chain;
    VecInit(&chain);
    EmitDummyStore(b, slot, td, &chain);
    free(chain.items);
}

/* Drops the owning value in `slot`, typed `td`: ^T/string free the pointer
   (and a boxed owning struct's fields first); T[] frees the backing buffer and
   drops owning elements via a loop (owning-struct elements included); an
   owning-struct slot drops its fields in place (no load, no free of the slot
   itself). freeArrayBuffer=false (vararg rest) drops elements but not the
   caller's stack buffer. */
static void EmitDropOneInternal(Builder* b, LLVMValueRef slot, TypeDesc td, bool freeArrayBuffer)
{
    if (TypeDescHasArrayShape(&td))
    {
        LLVMValueRef dataPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, slot), "adrop");

        if (td.arrayInner && BuilderIsOwningValue(b, td.arrayInner))
        {
            LLVMValueRef lenVal = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, slot), "adlen"));
            TypeDesc elemTd = Resolve(b, td.arrayInner);
            LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

            LLVMBasicBlockRef cond = NewBb(b, "adrop.cond");
            LLVMBasicBlockRef body = NewBb(b, "adrop.body");
            LLVMBasicBlockRef end = NewBb(b, "adrop.end");

            LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "adrop.i");
            LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
            Br(b, cond);

            PositionAtEnd(b, cond);
            LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
            LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, lenVal, "adlt");
            LLVMBuildCondBr(b->m_builder, cont, body, end);
            b->m_terminated = true;

            PositionAtEnd(b, body);
            LLVMValueRef epIdx[1] = {i};
            LLVMValueRef elemPtr = LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, epIdx, 1, "aelem");
            EmitDropOneInternal(b, elemPtr, elemTd, true);
            LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "next");
            LLVMBuildStore(b->m_builder, next, iSlot);
            Br(b, cond);

            PositionAtEnd(b, end);
        }

        if (freeArrayBuffer)
        {
            LLVMValueRef args[1] = {dataPtr};
            StrataFreeFn(b);
            LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, args, 1, "");
            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), ArrayDataPtr(b, slot));
            LLVMBuildStore(b->m_builder, LLVMConstInt(I32Ty(b), 0, 0), ArrayLenPtr(b, slot));
            LLVMBuildStore(b->m_builder, LLVMConstInt(I32Ty(b), 0, 0), ArrayCapPtr(b, slot));
        }

        return;
    }

    /* An owning struct held BY VALUE (an inline `Rec[]` element, a `Rec`
        field): drop its fields in place through the type's drop fn. The slot
        itself is inline storage — never loaded, never freed — and is zeroed
        so a later re-drop (element write, resize shrink) is a no-op. */
    if (td.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, td.structTypeName))
    {
        LLVMValueRef dropFn = GetOrCreateStructDropFn(b, td.structTypeName);
        LLVMTypeRef dropParams[2] = {b->m_ptrTy, b->m_ptrTy};
        LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), dropParams, 2, 0);
        LLVMValueRef dropArgs[2] = {slot, CurrentContext(b)};
        LLVMBuildCall2(b->m_builder, dropFnTy, dropFn, dropArgs, 2, "");
        LLVMBuildStore(b->m_builder, LLVMConstNull(td.type), slot);

        return;
    }

    /* box / string */
    LLVMValueRef ptr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slot, "box");

    /* Skip if null (e.g. moved-out binding). Mirrors the C backend's
       `if (v) { ... }` guard. */
    LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, ptr, LLVMConstNull(b->m_ptrTy), "null");
    LLVMBasicBlockRef skipBB = NewBb(b, "drop.skip");
    LLVMBasicBlockRef doBB = NewBb(b, "drop.do");
    LLVMBasicBlockRef endBB = NewBb(b, "drop.end");
    LLVMBuildCondBr(b->m_builder, isNull, skipBB, doBB);
    b->m_terminated = true;

    PositionAtEnd(b, doBB);

    /* A box of an owning inner owns that inner too: the cell at `ptr` is the
       inner's slot, so drop it through the same helper (an owning struct
       drops its fields, a string/T[] its buffer and owning elements, a nested
       box its own cell - all the way down). A plain value (int) needs no
       inner drop. */
    if (td.boxInner && BuilderIsOwningValue(b, td.boxInner))
    {
        EmitDropOneInternal(b, ptr, Resolve(b, td.boxInner), true);
    }

    LLVMValueRef args[1] = {ptr};
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, args, 1, "");
    Br(b, endBB);

    PositionAtEnd(b, skipBB);
    Br(b, endBB);

    PositionAtEnd(b, endBB);
    LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), slot);
}

void EmitDropOne(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    EmitDropOneInternal(b, slot, td, true);
}

/* Drops owning ELEMENTS of a stack-backed T[] (vararg rest) without freeing the buffer. */
void EmitDropOneStack(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    EmitDropOneInternal(b, slot, td, false);
}

/* Drops the owning locals declared from `fromIndex` on, the last declared first, so a local can rely on every
   local declared before it still being alive when it is dropped. */
void EmitDrops(Builder* b, size_t fromIndex)
{
    for (size_t i = b->m_owningLocals.count; i-- > fromIndex;)
    {
        OwnLocal* ol = (OwnLocal*)VecGet(&b->m_owningLocals, i);

        if (ol->stackBuffer)
        {
            EmitDropOneStack(b, ol->slot, ol->td);
        }
        else
        {
            EmitDropOne(b, ol->slot, ol->td);
        }
    }
}

/* ---- Owning temporaries ----
   A fresh owning call result that is only BORROWED inside a larger expression
   (`mk().length`, `int v = mkb();`, `substring(mks(), 0, 1)`) has no binding
   to drop it. EmitCall records such results in m_freshOwned; a borrow site
   that reads a value out of one parks it in a zeroed entry slot on m_temps,
   and the enclosing full statement (or condition) drops it via FlushTemps.
   When the value read out is itself OWNING (`mkArr()[0]`, `mkS().name`), the
   borrow site also records where that piece lives (m_tempParts); moving the
   piece nulls it there (EmitLValueForNullStore, ArgAddress), so the drop
   frees the rest of the temporary but not the moved piece. A fresh result
   that is moved whole (bound, returned, stored, passed as an owned argument)
   never reaches a borrow site, so it is never registered. */
void NoteFreshOwned(Builder* b, Value v)
{
    if (v.value && TypeDescIsOwning(b, &v.typeDesc))
    {
        VecPush(&b->m_freshOwned, (void*)v.value);
    }
}

const TempPart* FindTempPart(Builder* b, const Node* node)
{
    for (size_t i = b->m_tempParts.count; i-- > 0;)
    {
        const TempPart* tp = (const TempPart*)VecGet(&b->m_tempParts, i);

        if (tp->node == node)
        {
            return tp;
        }
    }

    return NULL;
}

static void AddTempPart(Builder* b, const Node* node, LLVMValueRef addr, TypeDesc td, size_t tempIdx)
{
    TempPart* tp = (TempPart*)arena_alloc(b->m_arena, sizeof(TempPart));
    tp->node = node;
    tp->addr = addr;
    tp->td = td;
    tp->tempIdx = tempIdx;
    VecPush(&b->m_tempParts, tp);
}

/* `node` read an owning piece out of `base` (evaluated from `baseNode`). If
   `base` is a fresh owning temporary (or itself a piece of one), claim it and
   record the piece so a move can null it. The piece lives at `heapAddr`
   (array element buffer, box cell) or, when that is NULL, at field
   `fieldIdx` of the by-value struct `structTy` parked in the temporary (or
   in the parent piece). Otherwise nothing is recorded and the temporary is
   left alone (a leak, never a double free). */
// clang-format off
void BorrowTempPart(Builder* b, Value base, const Node* baseNode, const Node* node, TypeDesc pieceTd,
                           LLVMValueRef heapAddr, LLVMTypeRef structTy, int fieldIdx)
{
    const TempPart* parent = FindTempPart(b, baseNode);
    LLVMValueRef baseAddr = parent ? parent->addr : NULL;
    size_t tempIdx = parent ? parent->tempIdx : b->m_temps.count;

    if (!parent)
    {
        baseAddr = ClaimTemp(b, base);

        if (!baseAddr)
        {
            return;
        }
    }

    LLVMValueRef addr = heapAddr;

    if (!addr)
    {
        /* Field of a by-value struct: address it inside the parked copy. */
        LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, (unsigned)fieldIdx)};
        addr = LLVMBuildGEP2(b->m_builder, structTy, baseAddr, idxs, 2, "tmpf");
    }

    AddTempPart(b, node, addr, pieceTd, tempIdx);
}
// clang-format on

/* `src` is borrowed to produce a value of type `resultTd`: if `src` is a fresh
   owning call result and the result owns nothing, schedule `src`'s drop at
   the end of the current statement. Owning results go through
   BorrowTempPart instead, which also records where the piece lives. */
void BorrowTemp(Builder* b, Value src, TypeDesc resultTd)
{
    if (TypeDescIsOwning(b, &resultTd))
    {
        return;
    }

    ClaimTemp(b, src);
}

/* If `src` is a fresh owning call result, park it in a zeroed slot scheduled
   for drop at the end of the current statement and return that slot;
   otherwise return NULL. */
LLVMValueRef ClaimTemp(Builder* b, Value src)
{
    for (size_t i = 0; i < b->m_freshOwned.count; i++)
    {
        if ((LLVMValueRef)VecGet(&b->m_freshOwned, i) != src.value)
        {
            continue;
        }

        /* Claimed once: swap-remove so a second borrow cannot register it again. */
        b->m_freshOwned.items[i] = b->m_freshOwned.items[b->m_freshOwned.count - 1];
        b->m_freshOwned.count--;

        /* Zeroed at function entry: a temp created on a conditional path
           (`a && mk().length > 0`) is null wherever it was not created, and
           each drop re-nulls it for the next loop iteration. */
        LLVMValueRef slot = EntryAllocaZeroed(b, src.typeDesc.type, "tmp");
        LLVMBuildStore(b->m_builder, src.value, slot);

        OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
        ol->slot = slot;
        ol->td = src.typeDesc;
        ol->stackBuffer = false;
        VecPush(&b->m_temps, ol);
        return slot;
    }

    return NULL;
}

/* Parks an owned value that nothing else will drop in a slot dropped at the end of the statement:
   an owned temporary handed to a `ref` parameter, which only borrows it. */
LLVMValueRef ParkTemp(Builder* b, Value v)
{
    LLVMValueRef claimed = ClaimTemp(b, v);

    if (claimed)
    {
        return claimed;
    }

    LLVMValueRef slot = EntryAllocaZeroed(b, v.typeDesc.type, "tmp");
    LLVMBuildStore(b->m_builder, v.value, slot);

    OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
    ol->slot = slot;
    ol->td = v.typeDesc;
    ol->stackBuffer = false;
    VecPush(&b->m_temps, ol);
    return slot;
}

/* Drops the temporaries registered since `mark` (end of a full statement or
   of a branch condition) and forgets unclaimed fresh results: past this point
   they were either moved or dropped by their consumer. */
void FlushTemps(Builder* b, size_t mark)
{
    if (!b->m_terminated)
    {
        for (size_t i = mark; i < b->m_temps.count; i++)
        {
            OwnLocal* ol = (OwnLocal*)VecGet(&b->m_temps, i);
            EmitDropOne(b, ol->slot, ol->td);
        }
    }

    b->m_temps.count = mark;
    b->m_freshOwned.count = 0;

    /* Pieces of the dropped temporaries are gone too. */
    size_t kept = 0;

    for (size_t i = 0; i < b->m_tempParts.count; i++)
    {
        TempPart* tp = (TempPart*)VecGet(&b->m_tempParts, i);

        if (tp->tempIdx < mark)
        {
            b->m_tempParts.items[kept++] = tp;
        }
    }

    b->m_tempParts.count = kept;
}

/* Resets the per-function temporary state (a new function body begins). */
void ResetTemps(Builder* b)
{
    b->m_temps.count = 0;
    b->m_freshOwned.count = 0;
    b->m_tempParts.count = 0;
}

/* What the checker decided about a use of this place; UseUnknown for anything it does not annotate. */
static UseKind CheckerUseOf(const Node* n)
{
    switch (n->kind)
    {
    case NodeIdent:
        return ((const IdentExpr*)n)->useKind;
    case NodeMember:
        return ((const MemberExpr*)n)->useKind;
    case NodeIndex:
        return ((const IndexExpr*)n)->useKind;
    default:
        return UseUnknown;
    }
}

/* The place a consuming use empties, or NULL. The checker decides what every use does (UseKind): only a move
   empties the place. A read or a copy leaves it alone,
   and so does a node the checker never reached (nothing of that kind exists in a program that compiles). */
Node* MovedSource(Node* n)
{
    Node* moved = (Node*)MovableBoxSourceNode(n);

    if (!moved)
    {
        return NULL;
    }

    UseKind use = CheckerUseOf(moved);

    return use == UseMove ? moved : NULL;
}

void NullMovedSource(Builder* b, Node* n)
{
    Node* moved = MovedSource(n);

    if (!moved)
    {
        return;
    }

    LValue src = EmitLValueForNullStore(b, moved);

    NullOwningSlot(b, src);
}

/* Produces an owned value of `innerType` from an evaluated expr - the single
   point encapsulating ownership construction. Non-owning inner returned as-is;
   owning literal heap-copied, movable source taken + nulled, non-movable taken
   as-is. Used for vars, ^T inners, fields, returns. An owning-struct inner
   (a `Rec` value boxed into `^Rec` or moved into a cell) moves too. */
LLVMValueRef EmitOwnedValue(Builder* b, Value evaluated, Node* init, const TypeName* innerType)
{
    if (!BuilderIsOwningValue(b, innerType))
    {
        return evaluated.value;
    }

    if (evaluated.typeDesc.isCString)
    {
        /* A `cstring` value is borrowed bytes (a bare `const char*`): the
           owning destination gets its own heap copy (NUL at [len]). */
        return BuildOwnedStringFromCStr(b, evaluated.value);
    }

    if (init->kind == NodeStrLiteral)
    {
        /* A string literal evaluates to a borrowed constant fat {ptr, len, cap};
           an owning destination gets its own heap copy (NUL at [len]). */
        LLVMValueRef srcPtr = LLVMBuildExtractValue(b->m_builder, evaluated.value, 0, "slit.p");
        LLVMValueRef lenVal = LLVMBuildExtractValue(b->m_builder, evaluated.value, 1, "slit.l");

        return BuildOwnedStringFat(b, srcPtr, lenVal);
    }

    NullMovedSource(b, init);
    return evaluated.value;
}

Value EmitCopyValue(Builder* b, Value src, TypeDesc td)
{
    if (td.isString)
    {
        /* Deep copy: fresh buffer with the NUL terminator at [len]. */
        LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, src.value, 0, "cp.p");
        LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, src.value, 1, "cp.l");

        /* An empty `string?` has no buffer (null data): its copy is empty too, not an allocated empty string that
           would test as set. */
        if (TypeDescIsOptional(&td))
        {
            LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, sp, LLVMConstNull(b->m_ptrTy), "osn");
            LLVMValueRef resSlot = EntryAlloca(b, LLVMTypeOf(src.value), "optstrcp");
            LLVMBasicBlockRef nullBb = NewBb(b, "os.null");
            LLVMBasicBlockRef copyBb = NewBb(b, "os.copy");
            LLVMBasicBlockRef endBb = NewBb(b, "os.end");
            LLVMBuildCondBr(b->m_builder, isNull, nullBb, copyBb);
            b->m_terminated = true;

            PositionAtEnd(b, nullBb);
            LLVMBuildStore(b->m_builder, LLVMConstNull(LLVMTypeOf(src.value)), resSlot);
            Br(b, endBb);

            PositionAtEnd(b, copyBb);
            LLVMBuildStore(b->m_builder, BuildOwnedStringFat(b, sp, sl), resSlot);
            Br(b, endBb);

            PositionAtEnd(b, endBb);
            return ValueMake(LLVMBuildLoad2(b->m_builder, LLVMTypeOf(src.value), resSlot, "osconv"), td);
        }

        return ValueMake(BuildOwnedStringFat(b, sp, sl), td);
    }

    if (td.structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, td.structTypeName))
    {
        return EmitStructValueCopy(b, src, td);
    }

    if (TypeDescIsBox(&td) && !td.boxInner)
    {
        StrataStrdupFn(b);

        /* An empty `string?` (null) copies as empty, not as a set optional holding a fresh empty string. */
        if (TypeDescIsOptional(&td))
        {
            LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, src.value, LLVMConstNull(b->m_ptrTy), "scn");
            LLVMValueRef resSlot = EntryAlloca(b, b->m_ptrTy, "optscp");
            LLVMBasicBlockRef nullBb = NewBb(b, "sc.null");
            LLVMBasicBlockRef copyBb = NewBb(b, "sc.copy");
            LLVMBasicBlockRef endBb = NewBb(b, "sc.end");
            LLVMBuildCondBr(b->m_builder, isNull, nullBb, copyBb);
            b->m_terminated = true;

            PositionAtEnd(b, nullBb);
            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), resSlot);
            Br(b, endBb);

            PositionAtEnd(b, copyBb);
            LLVMValueRef sargs[1] = {src.value};
            LLVMBuildStore(b->m_builder,
                           LLVMBuildCall2(b->m_builder, b->m_strdupFnType, b->m_strdupFn, sargs, 1, "cpstr"), resSlot);
            Br(b, endBb);

            PositionAtEnd(b, endBb);
            return ValueMake(LLVMBuildLoad2(b->m_builder, b->m_ptrTy, resSlot, "sconv"), td);
        }

        LLVMValueRef args[1] = {src.value};
        LLVMValueRef copy = LLVMBuildCall2(b->m_builder, b->m_strdupFnType, b->m_strdupFn, args, 1, "cpstr");
        return ValueMake(copy, td);
    }

    if (TypeDescIsBox(&td) && td.boxInner)
    {
        /* An optional source may be EMPTY (null): copy null as null instead
           of dereferencing it. */
        if (TypeDescIsOptional(&td))
        {
            LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, src.value, LLVMConstNull(b->m_ptrTy), "ocn");
            LLVMValueRef resSlot = EntryAlloca(b, b->m_ptrTy, "optcp");
            LLVMBasicBlockRef nullBb = NewBb(b, "oc.null");
            LLVMBasicBlockRef copyBb = NewBb(b, "oc.copy");
            LLVMBasicBlockRef endBb = NewBb(b, "oc.end");
            LLVMBuildCondBr(b->m_builder, isNull, nullBb, copyBb);
            b->m_terminated = true;

            PositionAtEnd(b, nullBb);
            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), resSlot);
            Br(b, endBb);

            PositionAtEnd(b, copyBb);
            LLVMValueRef heap = EmitBoxCellCopy(b, src, td);
            LLVMBuildStore(b->m_builder, heap, resSlot);
            Br(b, endBb);

            PositionAtEnd(b, endBb);
            return ValueMake(LLVMBuildLoad2(b->m_builder, b->m_ptrTy, resSlot, "oconv"), td);
        }

        return ValueMake(EmitBoxCellCopy(b, src, td), td);
    }

    if (TypeDescHasArrayShape(&td))
    {
        TypeDesc elemTd = Resolve(b, td.arrayInner);
        LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;
        LLVMValueRef oldLen = LLVMBuildExtractValue(b->m_builder, src.value, 1, "cln");
        LLVMValueRef oldData = LLVMBuildExtractValue(b->m_builder, src.value, 0, "cdt");

        /* Copying an EMPTY optional array yields the canonical empty
         {null, 0} - never a {malloc(0), 0} that would test non-empty. */
        if (TypeDescIsOptional(&td))
        {
            LLVMValueRef isEmpty = LLVMBuildICmp(b->m_builder, LLVMIntEQ, oldLen, LLVMConstInt(I32Ty(b), 0, 0), "oae");
            LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "oacp");
            LLVMBasicBlockRef emptyBb = NewBb(b, "oa.empty");
            LLVMBasicBlockRef copyBb2 = NewBb(b, "oa.copy");
            LLVMBasicBlockRef endBb2 = NewBb(b, "oa.end");
            LLVMBuildCondBr(b->m_builder, isEmpty, emptyBb, copyBb2);
            b->m_terminated = true;

            PositionAtEnd(b, emptyBb);
            LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), slot);
            Br(b, endBb2);

            PositionAtEnd(b, copyBb2);
            Value copiedArr = EmitArrayValueCopy(b, src, td, elemTd, elemTy, oldLen, oldData);
            LLVMBuildStore(b->m_builder, copiedArr.value, slot);
            Br(b, endBb2);

            PositionAtEnd(b, endBb2);
            return ValueMake(LLVMBuildLoad2(b->m_builder, ArrayStructType(b), slot, "oaconv"), td);
        }

        return EmitArrayValueCopy(b, src, td, elemTd, elemTy, oldLen, oldData);
    }

    return src;
}

/* Emits copy(arg) returning a deep copy of an owning value. */
Value EmitCopyBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    Value v = EmitExpr(b, arg0);

    return EmitCopyValue(b, v, v.typeDesc);
}

/* Emits drop(arg) — destroys the owning argument immediately, as if it were
   moved into an empty `void drop(^T v) {}` whose owned param is dropped at
   scope exit. Although it works for un-blessed optionals too */
Value EmitDropBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(b->m_ctx);

    /* Drop the owning lvalue in place (frees recursively, then nulls the slot). */
    Node* moved = MovedSource(arg0);

    if (moved)
    {
        LValue src = EmitLValue(b, moved);

        if (src.valid)
        {
            EmitDropOne(b, src.ptr, src.typeDesc);
        }
        else
        {
            EmitExpr(b, arg0); /* error recovery: keep any side effects */
        }

        return ValueMake(LLVMGetUndef(voidTy), TypeDescMake(voidTy, TD_VOID, NULL));
    }

    /* Temporary (non-lvalue) owning value. A string literal is a static
       global with nothing to free; anything else (call result, array
       literal) is freshly owned — park it in a temp slot and drop it. */
    if (arg0->kind == NodeStrLiteral)
    {
        return ValueMake(LLVMGetUndef(voidTy), TypeDescMake(voidTy, TD_VOID, NULL));
    }

    Value v = EmitExpr(b, arg0);
    LLVMValueRef slot
        = EntryAlloca(b, TypeDescHasArrayShape(&v.typeDesc) ? ArrayStructType(b) : b->m_ptrTy, "drop.tmp");
    LLVMBuildStore(b->m_builder, v.value, slot);
    EmitDropOne(b, slot, v.typeDesc);

    return ValueMake(LLVMGetUndef(voidTy), TypeDescMake(voidTy, TD_VOID, NULL));
}
