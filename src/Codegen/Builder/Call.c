// Builder: Calls: function lookup, arguments, builtins, intrinsics, extern slots.
#include "Codegen/Builder/BuilderInternal.h"

const FunctionDecl* FindModuleFunction(const Module* module, const char* name)
{
    if (!name)
    {
        return NULL;
    }

    for (size_t i = 0; i < module->functions.count; i++)
    {
        const FunctionDecl* f = (const FunctionDecl*)VecGet((Vec*)&module->functions, i);

        if (strcmp(f->name, name) == 0)
        {
            return f;
        }
    }

    return NULL;
}

// Statically names the handle type of `n` WITHOUT emitting IR (used to detect
// impl property access before the regular lvalue paths). NULL when unknown.
const char* StaticExprTypeName(Builder* b, Node* n)
{
    if (!n)
    {
        return NULL;
    }

    switch (n->kind)
    {
    case NodeIdent:
    {
        IdentExpr* id = (IdentExpr*)n;
        Value* sym = (Value*)StrMapGet(&b->symbols, id->name);

        if (!sym)
        {
            sym = (Value*)StrMapGet(&b->globals, id->name);
        }

        if (!sym)
        {
            return NULL;
        }

        // Names a struct or handle (both carry structTypeName); chains and
        // property lookup branch on opacity downstream.
        if (sym->typeDesc.structTypeName)
        {
            return sym->typeDesc.structTypeName;
        }

        if ((TypeDescIsBox(&sym->typeDesc) || TypeDescIsOptional(&sym->typeDesc)) && sym->typeDesc.boxInner
            && TypeRegistryIsOpaque(&b->registry, sym->typeDesc.boxInner->name))
        {
            return sym->typeDesc.boxInner->name; // `T?` receiver
        }

        return NULL;
    }
    case NodeCall:
    {
        const FunctionDecl* fd = ((CallExpr*)n)->resolvedDecl;

        return (fd && TypeRegistryIsOpaque(&b->registry, fd->returnType.name)) ? fd->returnType.name : NULL;
    }
    case NodeCast:
    {
        const TypeName* t = &((CastExpr*)n)->type;

        return TypeRegistryIsOpaque(&b->registry, t->name) ? t->name : NULL;
    }
    case NodeNullTest:
        return StaticExprTypeName(b, ((NullTestExpr*)n)->operand); // `T?` -> T
    case NodeIndex:
    {
        // `cams[i].Prop` — the element type comes from the base's TypeDesc
        // (arrayInner points into the declaration's stable TypeName tree).
        IndexExpr* ix = (IndexExpr*)n;
        Node* baseNode = ix->baseNode;
        const TypeName* inner = NULL;

        if (baseNode->kind == NodeIdent)
        {
            IdentExpr* id = (IdentExpr*)baseNode;
            Value* sym = (Value*)StrMapGet(&b->symbols, id->name);

            if (!sym)
            {
                sym = (Value*)StrMapGet(&b->globals, id->name);
            }

            inner = sym ? sym->typeDesc.arrayInner : NULL;
        }
        else if (baseNode->kind == NodeMember)
        {
            MemberExpr* bm = (MemberExpr*)baseNode;
            const char* bn = StaticExprTypeName(b, bm->baseNode);

            if (bn && !TypeRegistryIsOpaque(&b->registry, bn))
            {
                int fidx = TypeRegistryFieldIndex(&b->registry, bn, bm->member);

                if (fidx >= 0)
                {
                    const StructType* st = TypeRegistryFind(&b->registry, bn);
                    FieldDecl* field = (FieldDecl*)VecGet((Vec*)&st->fields, (size_t)fidx);
                    inner = TypeNameHasArrayShape(&field->type) ? field->type.elem : NULL;
                }
            }
        }

        return (inner && TypeRegistryIsOpaque(&b->registry, inner->name)) ? inner->name : NULL;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;
        const char* baseName = StaticExprTypeName(b, m->baseNode);

        if (!baseName)
        {
            return NULL;
        }

        if (TypeRegistryIsOpaque(&b->registry, baseName))
        {
            // Member of a handle: only an impl property continues the chain.
            ImplPropEntry* e
                = (ImplPropEntry*)StrMapGet(&b->implProps, ArenaFormat(b->arena, "%s.%s", baseName, m->member));

            return (e && TypeRegistryIsOpaque(&b->registry, e->decl->returnType.name)) ? e->decl->returnType.name
                                                                                       : NULL;
        }

        int idx = TypeRegistryFieldIndex(&b->registry, baseName, m->member);

        if (idx < 0)
        {
            return NULL;
        }

        const StructType* st = TypeRegistryFind(&b->registry, baseName);
        FieldDecl* field = (FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx);
        const TypeName* inner
            = (TypeNameIsBox(&field->type) || TypeNameIsOptional(&field->type)) ? field->type.inner : &field->type;

        return (inner && TypeRegistryIsOpaque(&b->registry, inner->name)) ? inner->name : NULL;
    }
    default:
        return NULL;
    }
}

// The JIT's `__strata_ext_<name>` slot for an extern, created on first use: every `extern<T>`
// instantiation shares the generic's slot, since the host binds one function for all T.
LLVMValueRef ExternSlot(Builder* b, const char* name)
{
    LLVMValueRef slot = (LLVMValueRef)StrMapGet(&b->externSlots, name);

    if (!slot)
    {
        slot = LLVMAddGlobal(b->mod, b->ptrTy, ArenaFormat(b->arena, "__strata_ext_%s", name));
        LLVMSetInitializer(slot, LLVMConstNull(b->ptrTy));
        StrMapPut(&b->externSlots, name, (void*)slot);
    }

    return slot;
}

static LLVMValueRef ArgAddress(Builder* b, Node* arg)
{
    LValue lvalue = EmitLValue(b, arg);

    if (lvalue.valid && !lvalue.vectorLane)
    {
        return lvalue.ptr;
    }

    Value value = EmitExpr(b, arg);

    // A piece of a temporary (`f(mkNested()[1])`): pass its storage inside
    // the temporary, like an lvalue, so a callee that takes ownership nulls
    // it there and the temporary's drop does not free it again.
    const TempPart* tp = FindTempPart(b, MovableBoxSourceNode(arg));

    if (tp)
    {
        return tp->addr;
    }

    LLVMValueRef slot = EntryAlloca(b, value.typeDesc.type, "outarg");
    LLVMBuildStore(b->builder, value.value, slot);

    return slot;
}

// Applies C default argument promotions to a value passed through a bare
// extern varargs
static LLVMValueRef ApplyCVarargPromotion(Builder* b, Value v)
{
    // A string vararg crosses as char* (printf("%s", s) et al).
    if (TypeDescIsString(&v.typeDesc))
    {
        return BuildExternStringPtr(b, v);
    }

    LLVMTypeRef ty = v.typeDesc.type;

    if (ty == I1Ty(b))
    {
        return LLVMBuildZExt(b->builder, v.value, I32Ty(b), "vap");
    }

    if (ty == LLVMInt8TypeInContext(b->ctx) || ty == LLVMInt16TypeInContext(b->ctx))
    {
        return TypeDescIsUnsigned(&v.typeDesc) ? LLVMBuildZExt(b->builder, v.value, I32Ty(b), "vap")
                                               : LLVMBuildSExt(b->builder, v.value, I32Ty(b), "vap");
    }

    if (ty == LLVMFloatTypeInContext(b->ctx))
    {
        return LLVMBuildFPExt(b->builder, v.value, LLVMDoubleTypeInContext(b->ctx), "vap");
    }

    return v.value;
}

// addressof(x): x's storage (a box's cell), or an extern / `@export` function's entry point.
static Value EmitAddressofBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc ptrTd = ResolveByName(b, STRATA_PTR_TYPE);
    const FunctionDecl* fn = n->addressOfFunction;

    if (fn)
    {
        // A JIT extern is reached through its slot, which the host binds.
        if (b->jitMode && fn->isExtern)
        {
            return ValueMake(LLVMBuildLoad2(b->builder, b->ptrTy, ExternSlot(b, fn->name), "fnaddr"), ptrTd);
        }

        FuncInfo* info = (FuncInfo*)StrMapGet(&b->funcs, fn->mangledName);

        return ValueMake(info ? info->function : LLVMConstNull(b->ptrTy), ptrTd);
    }

    Node* arg = (Node*)VecGet(&n->args, 0);

    while (arg->kind == NodeCast)
    {
        arg = ((CastExpr*)arg)->operand;
    }

    LValue lv = EmitLValue(b, arg);

    if (!lv.valid || lv.vectorLane)
    {
        if (b->diag)
        {
            DiagError(b->diag, arg->range, "cannot take the address of this expression");
        }

        return ValueMake(LLVMConstNull(b->ptrTy), ptrTd);
    }

    LLVMValueRef addr = lv.ptr;

    if (TypeDescIsBox(&lv.typeDesc))
    {
        addr = LLVMBuildLoad2(b->builder, b->ptrTy, lv.ptr, "cell");
    }

    return ValueMake(addr, ptrTd);
}

// sizeof<T>() / alignof<T>(): T's C size and alignment, as ulong constants.
static Value EmitSizeofBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    LLVMTypeRef ty = TypeDescHasArrayShape(&td) ? ArrayStructType(b) : td.type;
    TypeDesc ulongTd = TypeDescMake(I64Ty(b), TDF_UNSIGNED, NULL);

    if (strcmp(n->callee, "alignof") == 0)
    {
        // offsetof({ i8, T }, 1)
        LLVMTypeRef members[2] = {LLVMInt8TypeInContext(b->ctx), ty};
        LLVMTypeRef probe = LLVMStructTypeInContext(b->ctx, members, 2, 0);
        LLVMValueRef idx[2] = {LLVMConstInt(I32Ty(b), 0, 0), LLVMConstInt(I32Ty(b), 1, 0)};
        LLVMValueRef gep = LLVMConstGEP2(probe, LLVMConstNull(b->ptrTy), idx, 2);

        return ValueMake(LLVMConstPtrToInt(gep, I64Ty(b)), ulongTd);
    }

    return ValueMake(SizeOfConst(b, ty), ulongTd);
}

// load<T>(p): a T read from raw memory (no alignment assumed).
static Value EmitLoadBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    Value p = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    LLVMValueRef v = LLVMBuildLoad2(b->builder, td.type, p.value, "load");
    LLVMSetAlignment(v, 1);

    return ValueMake(v, td);
}

// store(p, v): v written to raw memory as the call's T (no alignment assumed).
static Value EmitStoreBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    Value p = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value v = Coerce(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)), td);
    LLVMValueRef st = LLVMBuildStore(b->builder, v.value, p.value);
    LLVMSetAlignment(st, 1);

    return ValueMake(NULL, TypeDescMake(LLVMVoidTypeInContext(b->ctx), TDF_VOID, NULL));
}

static Value EmitIntrinsicCall(Builder* b, CallExpr* n, bool* isValid)
{
    static const IntrinsicDefinition intrinsics[] = {
        // Array calls
        {"array_push",        EmitArrayBuiltin    },
        {"array_pop",         EmitArrayBuiltin    },
        {"array_resize",      EmitArrayBuiltin    },
        {"array_remove",      EmitArrayBuiltin    },
        {"array_reserve",     EmitArrayBuiltin    },
        {"array_swap_remove", EmitArrayBuiltin    },
        {"array_replace",     EmitArrayBuiltin    },
        {"array_take",        EmitArrayBuiltin    },

        // Misc
        {"copy",              EmitCopyBuiltin     },
        {"drop",              EmitDropBuiltin     },

        // Raw memory
        {"addressof",         EmitAddressofBuiltin},
        {"sizeof",            EmitSizeofBuiltin   },
        {"alignof",           EmitSizeofBuiltin   },
        {"load",              EmitLoadBuiltin     },
        {"store",             EmitStoreBuiltin    },

        // Strings
        {"substring",         EmitSubstringBuiltin},

        // Vectors
        {"float2",            EmitVectorConstruct },
        {"float3",            EmitVectorConstruct },
        {"float4",            EmitVectorConstruct },

        {"dot",               EmitVectorDot       },
        {"cross",             EmitVectorCross     },

        {"reduce",            EmitVectorReduce    },
    };

    for (int i = 0; i < ARRAY_COUNT(intrinsics); i++)
    {
        const IntrinsicDefinition* pcall = &intrinsics[i];

        if (pcall != NULL && strcmp(pcall->name, n->callee) == 0)
        {
            (*isValid) = true;
            return pcall->callDef(b, n);
        }
    }

    Value value = {.value = NULL, .typeDesc = {0}};
    return value;
}

// One field of a struct initializer call (`Point(1, 2)`): the argument lands in
// field i of the aggregate being built.
static LLVMValueRef EmitStructInitField(Builder* b, const StructType* st, LLVMValueRef agg, size_t i, Node* argNode)
{
    FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, i);
    TypeDesc fieldTd = Resolve(b, &fieldDecl->type);

    Value rawArg = EmitExpr(b, argNode);
    Value argValue;

    // A string field takes a heap copy of a literal so the field can
    // be freed without freeing a (borrowed) string constant.
    if (TypeDescIsString(&fieldTd) && argNode->kind == NodeStrLiteral)
    {
        LLVMValueRef sp = LLVMBuildExtractValue(b->builder, rawArg.value, 0, "fslit.p");
        LLVMValueRef sl = LLVMBuildExtractValue(b->builder, rawArg.value, 1, "fslit.l");
        argValue = ValueMake(BuildOwnedStringFat(b, sp, sl), fieldTd);
    }
    else if (TypeDescIsBox(&fieldTd) && fieldTd.boxInner
             && !(TypeDescIsBox(&rawArg.typeDesc) && rawArg.typeDesc.boxInner
                  && strcmp(rawArg.typeDesc.boxInner->name, fieldTd.boxInner->name) == 0))
    {
        // ^T / T? field from a non-box value (bare T, braced struct
        // literal): allocate a T slot and construct the value into it.
        // A braced `{}` against a `T?` field constructs a NON-EMPTY
        // boxed T (same rule as the named-literal form).
        LLVMValueRef heap = EmitBoxCell(b, fieldTd.boxInner, rawArg, argNode, "argbox");
        argValue = ValueMake(heap, fieldTd);
    }
    else
    {
        argValue = Coerce(b, rawArg, fieldTd);
    }

    agg = InsertStructField(b, st, agg, (int)i, argValue.value);

    // If an owning field was moved from an owning lvalue source,
    // null the source so its scope-exit drop is a no-op.
    if (FieldInitMovesSource(b, fieldTd, rawArg.typeDesc, argNode))
    {
        NullMovedSource(b, argNode);
    }

    return agg;
}

// Struct initializer call: `Point(1, 2)` builds the aggregate field by field.
static Value EmitStructInitCall(Builder* b, CallExpr* n)
{
    TypeName tn = MakeTypeName(b, n->callee);
    TypeDesc typeDesc = Resolve(b, &tn);

    const StructType* st = TypeRegistryFind(&b->registry, n->callee);

    // Fields the call leaves out keep their declared defaults, as in a struct literal.
    LLVMValueRef agg = DefaultValueOf(b, &tn, typeDesc);

    size_t nargs = n->args.count;

    for (size_t i = 0; i < nargs && st; i++)
    {
        if (i >= st->fields.count)
        {
            break;
        }

        agg = EmitStructInitField(b, st, agg, i, (Node*)VecGet(&n->args, i));
    }

    return ValueMake(agg, typeDesc);
}

// A typed rest param: the trailing call args are gathered into one T[] whose
// buffer is stack-allocated. A `ref` rest borrows owning elements instead of
// moving them.
static LLVMValueRef EmitTypedRestArg(Builder* b, CallExpr* n, const FunctionDecl* fd)
{
    const ParamDecl* restParam = (ParamDecl*)VecGet(&fd->params, fd->params.count - 1);
    const TypeName* elemType = TypeNameArrayElem(&restParam->type);

    if (!elemType)
    {
        TypeName empty = MakeTypeName(b, "");
        elemType = (const TypeName*)ArenaDup(b->arena, &empty, sizeof(TypeName));
    }

    Vec tail;
    VecInit(&tail);

    for (size_t i = fd->params.count - 1; i < n->args.count; i++)
    {
        VecPush(&tail, VecGet(&n->args, i));
    }

    Value arr = EmitArrayFromNodes(b, elemType, &tail, true, restParam->mod == ModRef);

    LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "rest");
    LLVMBuildStore(b->builder, arr.value, slot);

    return slot;
}

// Return-param slot: a fresh temp the callee writes its result into; loaded
// back into a value after the call.
static LLVMValueRef EmitReturnParamSlot(Builder* b, const FunctionDecl* fd, TypeDesc* outTd)
{
    *outTd = Resolve(b, &fd->returnType);

    return EntryAlloca(b, outTd->type, "retparam");
}

// Extern `T[]` param with a non-owning element: the array decays to a pointer
// to its first element.
static bool IsExternArrayDecayArg(Builder* b, const FunctionDecl* fd, size_t k)
{
    if (!(fd && fd->isExtern && k < fd->params.count))
    {
        return false;
    }

    const ParamDecl* pd = (ParamDecl*)VecGet(&fd->params, k);

    if (pd->mod != ModNone)
    {
        return false;
    }

    const TypeName* pty = &pd->type;
    const TypeName* elem = TypeNameArrayElem(pty);

    return TypeNameIsDynamicArray(pty) && !(elem && BuilderIsOwningValue(b, elem));
}

// `extern` array decay - T[] in extern functions are punned to T*
static LLVMValueRef LowerExternArrayDecayArg(Builder* b, Node* argNode)
{
    LLVMValueRef arrAddr = ArgAddress(b, argNode);
    LLVMValueRef dataSlot = ArrayDataPtr(b, arrAddr);

    return LLVMBuildLoad2(b->builder, b->ptrTy, dataSlot, "arrptr");
}

// Array literal against a dynamic-array param: the param's shape
// stamps the element types (sema), so the literal can construct.
static LLVMValueRef LowerArrayLiteralArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode,
                                         Vec* ownedArgTemps)
{
    const ParamDecl* litParam = (ParamDecl*)VecGet(&fd->params, k);
    ArrayInitExpr* ai = AS_NODE(ArrayInitExpr, argNode);
    Value arr;

    if (litParam->mod == ModRef)
    {
        // `ref T[]` borrows: construct the literal on the STACK
        // (a la the typed rest slot). The caller drops owning
        // elements after the call; the stack buffer is never
        // freed.
        arr = EmitArrayFromNodes(b, ai->elementType, &ai->elements, true, false);
    }
    else
    {
        // By value: a normal heap-backed dynamic array — the
        // callee owns it and drops it at exit.
        arr = EmitArrayInit(b, ai);
    }

    LLVMValueRef litSlot = EntryAlloca(b, ArrayStructType(b), "litarr");
    LLVMBuildStore(b->builder, arr.value, litSlot);

    if (litParam->mod == ModRef)
    {
        OwnedArgTemp* temp = (OwnedArgTemp*)ArenaAlloc(b->arena, sizeof(OwnedArgTemp));
        temp->slot = litSlot;
        temp->td = arr.typeDesc;
        temp->stackOnly = true;
        VecPush(ownedArgTemps, temp);
    }

    return litSlot;
}

// Fixed array → `ref T[]` param: borrow the inline storage as a
// stack fat {&fixed[0], N, N} — a slice/view with no allocation and
// no copy (same stack-fat trick as the typed rest slot). The callee
// reads/writes elements through the data pointer; a `ref` binding is
// never dropped or freed (ref params are exempt from the
// owning-local teardown). Returns NULL when the argument is not a fixed
// array, so the caller lowers it the ordinary way.
static LLVMValueRef TryLowerFixedArrayViewArg(Builder* b, const FunctionDecl* fd, size_t k, bool shouldPassByPtr,
                                              Node* argNode)
{
    if (!(shouldPassByPtr && fd && k < fd->params.count && (argNode->kind == NodeIdent || argNode->kind == NodeMember)))
    {
        return NULL;
    }

    const ParamDecl* viewParam = (ParamDecl*)VecGet(&fd->params, k);

    if (!(viewParam->mod == ModRef && TypeNameIsDynamicArray(&viewParam->type)))
    {
        return NULL;
    }

    LValue lv = EmitLValue(b, argNode);

    if (!(lv.valid && TypeDescIsFixedArray(&lv.typeDesc)))
    {
        return NULL;
    }

    LLVMValueRef zero[2] = {LLVMConstInt(I64Ty(b), 0, 0), LLVMConstInt(I64Ty(b), 0, 0)};
    LLVMValueRef data = LLVMBuildGEP2(b->builder, lv.typeDesc.type, lv.ptr, zero, 2, "fixview");
    LLVMValueRef len = LLVMConstInt(I32Ty(b), (unsigned long long)lv.typeDesc.fixedLength, 0);
    LLVMValueRef fat = LLVMGetUndef(ArrayStructType(b));
    fat = LLVMBuildInsertValue(b->builder, fat, data, 0, "fixview.p");
    fat = LLVMBuildInsertValue(b->builder, fat, len, 1, "fixview.l");
    fat = LLVMBuildInsertValue(b->builder, fat, len, 2, "fixview.c");

    LLVMValueRef viewSlot = EntryAlloca(b, ArrayStructType(b), "fixview");
    LLVMBuildStore(b->builder, fat, viewSlot);

    return viewSlot;
}

// Internal string param (by slot address): the callee owns the
// fat and drops/nulls the slot at exit. Dispatch on the EMITTED
// value's type, never the arg's AST shape: a `cstring` of ANY
// form (element, member chain, call result, cast) is borrowed
// bytes that must be heap-copied into a fresh owned fat.
static LLVMValueRef LowerInternalStringArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode)
{
    LValue lv = EmitLValue(b, argNode);
    // A `ref string` param only borrows: owned temporaries made for it die with the statement.
    const ParamDecl* strParam = (const ParamDecl*)VecGet((Vec*)&fd->params, k);
    bool borrowsString = strParam->mod == ModRef;

    // A string lvalue: pass the caller's own storage (move
    // semantics — the callee nulls the slot at exit).
    if (lv.valid && !lv.vectorLane && !TypeDescIsCString(&lv.typeDesc))
    {
        return lv.ptr;
    }

    Value av = lv.valid ? ValueMake(LLVMBuildLoad2(b->builder, lv.typeDesc.type, lv.ptr, "csarg"), lv.typeDesc)
                        : EmitExpr(b, argNode);

    if (TypeDescIsCString(&av.typeDesc))
    {
        LLVMValueRef owned = BuildOwnedStringFromCStr(b, av.value);

        if (borrowsString)
        {
            return ParkTemp(b, ValueMake(owned, Resolve(b, &strParam->type)));
        }

        LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "cs2s");
        LLVMBuildStore(b->builder, owned, slot);

        return slot;
    }

    const TempPart* tp = FindTempPart(b, MovableBoxSourceNode(argNode));

    if (tp)
    {
        // A string piece of a temporary (`f(mkArr()[0])`):
        // hand over its storage inside the temporary, like
        // an lvalue, so the callee's null-at-exit keeps the
        // temporary's drop from freeing it again.
        return tp->addr;
    }

    if (borrowsString)
    {
        return ParkTemp(b, av);
    }

    // A non-lvalue string value (e.g. a call result):
    // spill the already-computed fat (ArgAddress's
    // fallback, minus a second evaluation).
    LLVMValueRef slot = EntryAlloca(b, av.typeDesc.type, "outarg");
    LLVMBuildStore(b->builder, av.value, slot);

    return slot;
}

// Internal string param (by slot address) given a string literal: the callee
// owns and drops the value, so materialize an owned fat into a fresh temp slot
// instead of handing out the borrowed constant.
static LLVMValueRef LowerStringLiteralParamArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode)
{
    Value litVal = EmitExpr(b, argNode);
    const ParamDecl* litParam = fd && k < fd->params.count ? (const ParamDecl*)VecGet((Vec*)&fd->params, k) : NULL;
    LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "stslot");

    if (litParam && litParam->mod == ModRef)
    {
        // A `ref string` param only reads: lend it the constant itself.
        LLVMBuildStore(b->builder, litVal.value, slot);
    }
    else
    {
        LLVMValueRef sp = LLVMBuildExtractValue(b->builder, litVal.value, 0, "arglit.p");
        LLVMValueRef sl = LLVMBuildExtractValue(b->builder, litVal.value, 1, "arglit.l");
        LLVMBuildStore(b->builder, BuildOwnedStringFat(b, sp, sl), slot);
    }

    return slot;
}

// A box argument to a by-pointer param whose param is not a box: the callee
// borrows the box's contents. A box that is only a temporary (`F(Box(1))`) has
// no other owner: park it so the end of the statement drops it.
static LLVMValueRef LowerBorrowedBoxArg(Builder* b, Node* argNode)
{
    Value boxArg = EmitExpr(b, argNode);

    if (argNode->kind != NodeIdent && argNode->kind != NodeMember && argNode->kind != NodeIndex
        && TypeDescIsOwning(b, &boxArg.typeDesc))
    {
        ParkTemp(b, boxArg);
    }

    return boxArg.value;
}

// Extern string param: the C side only borrows, so an owned temporary dies with
// the statement. A `string?` param keeps the raw pointer so NULL stays NULL.
static LLVMValueRef LowerExternStringArg(Builder* b, Value av, bool paramIsRealBox)
{
    if (paramIsRealBox)
    {
        // `string?` param: pass the RAW data pointer — NULL stays
        // NULL (empty optional), preserving the C-side NULL check.
        return LLVMBuildExtractValue(b->builder, av.value, 0, "optstr");
    }

    // Extern string param: pun the fat to char*
    // (NUL-terminated; empty {null, 0} materializes a valid
    // empty C string).
    ClaimTemp(b, av); // C only borrows: an owned temporary dies with the statement

    return BuildExternStringPtr(b, av);
}

// Extern array argument to a `^T` / `T?` param: the T[] / T[N] is passed as a
// pointer to its first element (the box IS that pointer). Dynamic arrays hand
// over their data buffer; fixed arrays GEP to element 0. Returns NULL when the
// element type does not match, so the caller lowers the argument normally.
static LLVMValueRef TryLowerExternArrayToBoxArg(Builder* b, const TypeName* extTy, Value av, Node* argNode)
{
    const TypeName* boxInner = TypeNameBoxInner(extTy);
    const TypeName* argElem = av.typeDesc.arrayInner;

    if (!(boxInner && argElem && strcmp(boxInner->name, argElem->name) == 0))
    {
        return NULL;
    }

    if (TypeDescHasArrayShape(&av.typeDesc))
    {
        return LLVMBuildExtractValue(b->builder, av.value, 0, "arrptr");
    }

    LLVMValueRef slot = ArgAddress(b, argNode);
    LLVMValueRef zero[1] = {LLVMConstInt(I64Ty(b), 0, 0)};

    return LLVMBuildGEP2(b->builder, av.typeDesc.type, slot, zero, 1, "farr");
}

// A plain value passed to an extern `^T` / `T?` param: a real box (or optional)
// takes a fresh cell holding the owned value; a plain `string` param takes the
// value itself as the char*.
static LLVMValueRef LowerExternValueToBoxArg(Builder* b, Node* argNode, Value av, const TypeName* extTy,
                                             bool paramIsRealBox)
{
    const TypeName* innerTn = TypeNameBoxInner(extTy) ? TypeNameBoxInner(extTy) : StringTypeName(b);

    if (!paramIsRealBox)
    {
        // Plain `string` param: the value already is the char*.
        return av.value;
    }

    LLVMValueRef owned = EmitOwnedValue(b, av, argNode, innerTn);

    LLVMValueRef cellSz = SizeOfConst(b, b->ptrTy);
    LLVMValueRef cellArgs[1] = {cellSz};
    StrataAllocFn(b);
    LLVMValueRef cell = LLVMBuildCall2(b->builder, b->allocFnType, b->allocFn, cellArgs, 1, "argcell");
    LLVMBuildStore(b->builder, owned, cell);

    return cell;
}

// Extern ^T / T? param: pass the pointer ITSELF by value.
// A box/optional arg already is that pointer; a plain owning
// value is constructed into a fresh cell first.
static LLVMValueRef LowerExternBoxParamArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode)
{
    const ParamDecl* extParam = (ParamDecl*)VecGet(&fd->params, k);
    const TypeName* extTy = &extParam->type;

    Value av = EmitExpr(b, argNode);

    if (TypeNameIsBox(extTy) && (TypeDescHasArrayShape(&av.typeDesc) || TypeDescIsFixedArray(&av.typeDesc)))
    {
        LLVMValueRef decayed = TryLowerExternArrayToBoxArg(b, extTy, av, argNode);

        if (decayed)
        {
            return decayed;
        }
    }

    bool paramIsRealBox = TypeNameIsBox(extTy) || TypeNameIsOptional(extTy);

    if (TypeDescIsString(&av.typeDesc))
    {
        return LowerExternStringArg(b, av, paramIsRealBox);
    }

    if (TypeDescIsBox(&av.typeDesc))
    {
        return paramIsRealBox ? av.value : DerefBoxValue(b, av).value;
    }

    return LowerExternValueToBoxArg(b, argNode, av, extTy, paramIsRealBox);
}

// C default argument promotions for a bare extern `...` trailing arg.
static LLVMValueRef LowerCVarargArg(Builder* b, Value v)
{
    // Bare extern `...` trailing args follow C default promotions.
    // C only borrows: an owned temporary (`printf("%s", mk())`) dies with the statement.
    ClaimTemp(b, v);

    return ApplyCVarargPromotion(b, DerefBoxValue(b, v));
}

// By-value (non-indirect) param: the value is coerced to the declared type; an
// owning struct is deep-copied so the caller keeps its own fields (C value
// semantics; `ref` is the zero-copy escape).
static LLVMValueRef LowerByValueArg(Builder* b, const FunctionDecl* fd, size_t k, Value v)
{
    // A box arg passed to a by-value (non-indirect) param - e.g. a
    // plain handle - must be dereferenced to its value, not passed
    // as the box's own heap pointer.
    v = DerefBoxValue(b, v);

    if (fd && k < fd->params.count)
    {
        // Implicit scalar widening/narrowing to the declared
        // param type (e.g. a float literal to a double param)
        // - without this the arg crosses at its own width and
        // the call gets a mismatched ABI (garbage).
        TypeDesc paramTd = Resolve(b, &((ParamDecl*)VecGet(&fd->params, k))->type);
        v = Coerce(b, v, paramTd);

        // An owning struct passed by value: the callee owns
        // and drops its copy at return, so hand it a deep
        // copy — the caller's value keeps its own fields
        // (C value semantics; `ref` is the zero-copy escape).
        if (paramTd.structTypeName && TypeRegistryIsOwningStruct(&b->registry, paramTd.structTypeName))
        {
            v = EmitCopyValue(b, v, paramTd);
        }
    }

    return v.value;
}

// Indirect (by-pointer) param given a plain expression: a `ref` param lent an
// owned temporary parks it for the statement; a call result is spilled (its
// value is already computed, and ArgAddress would evaluate it again); anything
// else passes the address of its storage.
static LLVMValueRef LowerByPtrPlainArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode, Value v,
                                       bool paramIsBoxType, bool argIsBox)
{
    const ParamDecl* refParam = fd && k < fd->params.count ? (const ParamDecl*)VecGet((Vec*)&fd->params, k) : NULL;
    bool borrowsTemp = refParam && refParam->mod == ModRef && !fd->isExtern && argNode->kind != NodeIdent
                       && argNode->kind != NodeMember && argNode->kind != NodeIndex && TypeDescIsOwning(b, &v.typeDesc);

    if (borrowsTemp)
    {
        // An owned temporary lent to a `ref` param: dropped when the statement ends.
        return ParkTemp(b, v);
    }

    if (argNode->kind == NodeCall)
    {
        // A call result has no address: spill the value already
        // computed above. ArgAddress would evaluate the call a
        // second time (repeating its side effects and leaking the
        // first result).
        LLVMValueRef slot = EntryAlloca(b, v.typeDesc.type, "outarg");
        LLVMBuildStore(b->builder, v.value, slot);

        return slot;
    }

    // A box-typed field or element (`h.buffer.Method()`, `ref T` param from a `^T`
    // / `T?` slot): the callee wants the boxed value, not the slot.
    LValue slot = {0};

    if (!paramIsBoxType && !argIsBox && (argNode->kind == NodeMember || argNode->kind == NodeIndex))
    {
        slot = EmitLValue(b, argNode);
    }

    if (slot.valid && !slot.vectorLane && TypeDescIsBox(&slot.typeDesc))
    {
        return LLVMBuildLoad2(b->builder, b->ptrTy, slot.ptr, "boxarg");
    }

    if (slot.valid && !slot.vectorLane)
    {
        return slot.ptr;
    }

    return ArgAddress(b, argNode);
}

// A plain (non-special-cased) argument: evaluated once, then passed by pointer,
// by value, or as a bare C vararg.
static LLVMValueRef LowerPlainArg(Builder* b, const FunctionDecl* fd, size_t k, Node* argNode, bool cVararg,
                                  bool shouldPassByPtr, bool paramIsBoxType, bool argIsBox)
{
    Value v = EmitExpr(b, argNode);

    if (cVararg && k >= fd->params.count)
    {
        return LowerCVarargArg(b, v);
    }

    if (shouldPassByPtr)
    {
        return LowerByPtrPlainArg(b, fd, k, argNode, v, paramIsBoxType, argIsBox);
    }

    return LowerByValueArg(b, fd, k, v);
}

// Whether a by-pointer argument names a box (a variable, or a call returning one):
// the callee then borrows the box's contents.
static bool CallArgIsBoxSource(Builder* b, Node* argNode)
{
    if (argNode->kind == NodeIdent)
    {
        Value* sym = (Value*)StrMapGet(&b->symbols, ((IdentExpr*)argNode)->name);

        if (!sym)
        {
            sym = (Value*)StrMapGet(&b->globals, ((IdentExpr*)argNode)->name);
        }

        return sym && TypeDescIsBox(&sym->typeDesc);
    }

    if (argNode->kind == NodeCall)
    {
        FuncInfo* fi = (FuncInfo*)StrMapGet(&b->funcs, ((CallExpr*)argNode)->callee);

        return fi && TypeDescIsBox(&fi->returnType);
    }

    return false;
}

// A string literal lent to a `ref cstring` param: the slot holds the constant's `.rodata`
// pointer. It is borrowed, so no owned `string` temporary is made for the statement to drop.
static LLVMValueRef LowerCStringLiteralRefArg(Builder* b, Node* argNode)
{
    LLVMValueRef slot = EntryAlloca(b, b->ptrTy, "cslit");
    LLVMBuildStore(b->builder, EmitCStringLiteralPtr(b, (const StrLiteral*)argNode), slot);

    return slot;
}

// The argument-only dispatcher: one call argument lowered to its slot form, by
// which parameter kind it binds to.
static LLVMValueRef EmitCallArg(Builder* b, CallExpr* n, FuncInfo* info, const FunctionDecl* fd, size_t k, bool cVararg,
                                Vec* ownedArgTemps)
{
    bool shouldPassByPtr = k < info->paramByPtrCount && info->paramByPtr[k];
    Node* argNode = (Node*)VecGet(&n->args, k);

    // ^T coerced to T: if the param is a plain struct (not box),
    // and the arg is a box, the heap pointer IS the T* the param wants.
    bool paramIsBoxType
        = fd && k < fd->params.count && BuilderIsOwningType(b, &((ParamDecl*)VecGet(&fd->params, k))->type);
    bool argIsBox = shouldPassByPtr && !paramIsBoxType && CallArgIsBoxSource(b, argNode);

    if (IsExternArrayDecayArg(b, fd, k))
    {
        return LowerExternArrayDecayArg(b, argNode);
    }

    if (shouldPassByPtr && fd && k < fd->params.count && argNode->kind == NodeArrayInit
        && TypeNameIsDynamicArray(&((ParamDecl*)VecGet(&fd->params, k))->type))
    {
        return LowerArrayLiteralArg(b, fd, k, argNode, ownedArgTemps);
    }

    LLVMValueRef fixedView = TryLowerFixedArrayViewArg(b, fd, k, shouldPassByPtr, argNode);

    if (fixedView)
    {
        return fixedView;
    }

    if (shouldPassByPtr && paramIsBoxType && fd && k < fd->params.count && argNode->kind != NodeStrLiteral
        && TypeIsString(&b->registry, ((ParamDecl*)VecGet((Vec*)&fd->params, k))->type.name))
    {
        return LowerInternalStringArg(b, fd, k, argNode);
    }

    if (shouldPassByPtr && paramIsBoxType && argNode->kind == NodeStrLiteral)
    {
        return LowerStringLiteralParamArg(b, fd, k, argNode);
    }

    if (shouldPassByPtr && fd && k < fd->params.count && argNode->kind == NodeStrLiteral
        && TypeIsCString(&b->registry, ((ParamDecl*)VecGet((Vec*)&fd->params, k))->type.name))
    {
        return LowerCStringLiteralRefArg(b, argNode);
    }

    if (shouldPassByPtr && !paramIsBoxType && argIsBox)
    {
        return LowerBorrowedBoxArg(b, argNode);
    }

    if (!shouldPassByPtr && paramIsBoxType && fd && fd->isExtern && k < fd->params.count)
    {
        return LowerExternBoxParamArg(b, fd, k, argNode);
    }

    return LowerPlainArg(b, fd, k, argNode, cVararg, shouldPassByPtr, paramIsBoxType, argIsBox);
}

// Lowers every call argument into args[k]. A typed rest param gathers the
// trailing args into one array and a `return` param gets a fresh out-slot; both
// keep the LLVM call at exactly params.count args.
static void EmitCallArgs(Builder* b, CallExpr* n, FuncInfo* info, const FunctionDecl* fd, LLVMValueRef* args,
                         size_t nargs, Vec* ownedArgTemps, LLVMValueRef* returnParamSlot, TypeDesc* returnParamTd)
{
    bool hasReturnParam = fd && FunctionHasReturnParam(fd);
    bool typedRest = fd && fd->isVariadic && !fd->isCVararg && fd->params.count > 0;
    bool cVararg = fd && fd->isCVararg;

    for (size_t k = 0; k < nargs; k++)
    {
        if (hasReturnParam && k == fd->params.count - 1)
        {
            *returnParamSlot = EmitReturnParamSlot(b, fd, returnParamTd);
            args[k] = *returnParamSlot;
            continue;
        }

        if (typedRest && k == fd->params.count - 1)
        {
            args[k] = EmitTypedRestArg(b, n, fd);
            continue;
        }

        args[k] = EmitCallArg(b, n, info, fd, k, cVararg, ownedArgTemps);
    }
}

// The hidden context pointer (see DeclareFunction) is never written by Strata
// source; forward the CALLING function's pointer automatically. It goes first, so
// the final order is [ctx, self, ...rest].
static LLVMValueRef* PrependHiddenCtxArg(Builder* b, FuncInfo* info, LLVMValueRef* args, unsigned* count)
{
    if (!info->hasHiddenCtxParam)
    {
        return args;
    }

    size_t nargs = *count;
    LLVMValueRef* finalArgs = (LLVMValueRef*)ArenaAlloc(b->arena, (nargs + 1) * sizeof(LLVMValueRef));
    finalArgs[0] = b->curGlobalsPtr;

    if (nargs > 0)
    {
        memcpy(finalArgs + 1, args, nargs * sizeof(LLVMValueRef));
    }

    *count = (unsigned)(nargs + 1);

    return finalArgs;
}

// `extern<T>`: T's descriptor goes last, after every argument the call spells out.
static LLVMValueRef* AppendTypeDescArg(Builder* b, FuncInfo* info, const FunctionDecl* fd, LLVMValueRef* args,
                                       unsigned* count)
{
    if (!(info->hasTypeDescParam && fd && fd->typeArg))
    {
        return args;
    }

    unsigned finalCount = *count;
    LLVMValueRef* withTypeDesc = (LLVMValueRef*)ArenaAlloc(b->arena, (finalCount + 1) * sizeof(LLVMValueRef));

    if (finalCount > 0)
    {
        memcpy(withTypeDesc, args, finalCount * sizeof(LLVMValueRef));
    }

    withTypeDesc[finalCount] = TypeDescConstant(b, fd->typeArg);
    *count = finalCount + 1;

    return withTypeDesc;
}

// Through a delegate: the pointer value is the callee; a null one panics instead of jumping to 0.
static LLVMValueRef EmitDelegateCallee(Builder* b, CallExpr* n)
{
    LLVMValueRef callee = EmitExpr(b, n->delegateCallee).value;

    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, callee, LLVMConstNull(b->ptrTy), "dlgnull");
    LLVMBasicBlockRef nullBB = NewBb(b, "dlg.null");
    LLVMBasicBlockRef callBB = NewBb(b, "dlg.call");
    LLVMBuildCondBr(b->builder, isNull, nullBB, callBB);

    PositionAtEnd(b, nullBB);
    LLVMValueRef panicArgs[1]
        = {MsgGlobalPtr(b, ".pmsg", ArenaFormat(b->arena, "call through a null '%s' delegate", n->delegateDecl->name))};
    StrataPanicFn(b);
    LLVMBuildCall2(b->builder, b->panicFnType, b->panicFn, panicArgs, 1, "");
    LLVMBuildUnreachable(b->builder);

    PositionAtEnd(b, callBB);

    return callee;
}

// A JIT extern is called through its slot, which the host binds later.
static LLVMValueRef ResolveJitExternCallee(Builder* b, CallExpr* n, LLVMValueRef callee, bool* slotExtern)
{
    if (!b->jitMode)
    {
        return callee;
    }

    LLVMValueRef slot = (LLVMValueRef)StrMapGet(&b->externSlots, n->callee);

    if (!slot)
    {
        return callee;
    }

    *slotExtern = true;

    return LLVMBuildLoad2(b->builder, b->ptrTy, slot, "extfn");
}

// A JIT extern slot starts null: under nullExternCall, a call before the host
// binds it panics with the unbound name instead of jumping to address 0. The
// panic block RETURNS a zero value so a host panic handler that returns keeps the
// JIT frame's stack walker away.
static void EmitNullExternGuard(Builder* b, CallExpr* n, LLVMValueRef callee)
{
    LLVMValueRef isNull = LLVMBuildICmp(b->builder, LLVMIntEQ, callee, LLVMConstNull(b->ptrTy), "extnull");
    LLVMBasicBlockRef nullBB = NewBb(b, "ext.null");
    LLVMBasicBlockRef callBB = NewBb(b, "ext.call");
    LLVMBuildCondBr(b->builder, isNull, nullBB, callBB);
    b->terminated = true;

    PositionAtEnd(b, nullBB);
    {
        LLVMValueRef args2[1]
            = {MsgGlobalPtr(b, ".pmsg", ArenaFormat(b->arena, "call to null extern function '%s'", n->callee))};
        StrataPanicFn(b);
        LLVMBuildCall2(b->builder, b->panicFnType, b->panicFn, args2, 1, "");

        // The `ret` here terminates the ENCLOSING Strata function, so it
        // must use the enclosing function's return type (curRet), not the
        // extern's. Returning the extern's type (e.g. a ptr for a `handle`
        // return inside an i32 fn) produces malformed IR that crashes LLVM
        // optimizers.
        if (TypeDescIsVoid(&b->curRet))
        {
            LLVMBuildRetVoid(b->builder);
        }
        else
        {
            LLVMBuildRet(b->builder, LLVMConstNull(b->curRetAbi));
        }
    }

    PositionAtEnd(b, callBB);
}

static LLVMValueRef EmitCallInstruction(Builder* b, CallExpr* n, FuncInfo* info, LLVMValueRef callee,
                                        LLVMValueRef* finalArgs, unsigned finalCount, bool slotExtern)
{
    if (slotExtern && b->nullExternCheck)
    {
        EmitNullExternGuard(b, n, callee);
    }

    return LLVMBuildCall2(b->builder, info->type, callee, finalArgs, finalCount, "call");
}

// Array-literal args: `ref T[]` temps are stack views (drop owning
// elements, never the buffer); by-value temps transferred ownership to
// the callee and are not touched here.
static void DropOwnedArgTemps(Builder* b, Vec* ownedArgTemps)
{
    for (size_t t = 0; t < ownedArgTemps->count; t++)
    {
        OwnedArgTemp* at = (OwnedArgTemp*)VecGet(ownedArgTemps, t);

        if (at->stackOnly)
        {
            EmitDropOneStack(b, at->slot, at->td);
        }
        else
        {
            EmitDropOne(b, at->slot, at->td);
        }
    }
}

// Sub-32-bit integer extern returns (bool/byte/sbyte/short/ushort): the
// C host only defines the low part of the widened register (AL/AX), so
// narrow the raw result back to the semantic type - truncating away the
// undefined upper bits, then re-extending per the semantic signedness.
// Strata-emitted callees always extend the full register themselves.
static LLVMValueRef NarrowExternIntReturn(Builder* b, const FunctionDecl* fd, FuncInfo* info, LLVMValueRef call)
{
    if (!(fd && fd->isExtern && info->retSemanticTy && call
          && LLVMGetTypeKind(info->retSemanticTy) == LLVMIntegerTypeKind))
    {
        return call;
    }

    LLVMTypeRef abiRet = LLVMGetReturnType(info->type);

    if (!(abiRet && LLVMGetTypeKind(abiRet) == LLVMIntegerTypeKind
          && LLVMGetIntTypeWidth(abiRet) > LLVMGetIntTypeWidth(info->retSemanticTy)))
    {
        return call;
    }

    LLVMValueRef narrow = LLVMBuildTrunc(b->builder, call, info->retSemanticTy, "ret.narrow");

    bool zeroExtend = info->retSemanticUnsigned || info->retSemanticTy == I1Ty(b);

    return zeroExtend ? LLVMBuildZExt(b->builder, narrow, abiRet, "ret.zext")
                      : LLVMBuildSExt(b->builder, narrow, abiRet, "ret.sext");
}

// Extern `string` return: the host handed back a NUL-terminated char*;
// copy it into a fresh owned buffer (the semantic type stays the fat).
// A DISCARDED result is skipped entirely — no allocation, no leak.
static Value EmitExternStringResult(Builder* b, FuncInfo* info, LLVMValueRef call, bool discarded)
{
    if (discarded)
    {
        return ValueMake(LLVMConstNull(ArrayStructType(b)), info->returnType);
    }

    Value owned = ValueMake(BuildOwnedStringFromCStr(b, call), info->returnType);
    NoteFreshOwned(b, owned);

    return owned;
}

// The Strata-level result of a call, shaped by the callee's return kind.
static Value EmitCallResult(Builder* b, const FunctionDecl* fd, FuncInfo* info, LLVMValueRef call, bool discarded,
                            bool hasReturnParam, LLVMValueRef returnParamSlot, TypeDesc returnParamTd)
{
    // Return-param call: the C function wrote the result into our temp slot;
    // load it back as the Strata-level return value.
    if (hasReturnParam)
    {
        LLVMValueRef retVal = LLVMBuildLoad2(b->builder, returnParamTd.type, returnParamSlot, "retparam.val");

        return ValueMake(retVal, returnParamTd);
    }

    if (info->externStringReturn)
    {
        return EmitExternStringResult(b, info, call, discarded);
    }

    Value result = ValueMake(call, info->returnType);

    // A Strata function's owning result is a fresh value the caller owns
    // (extern results keep their existing host-ownership handling).
    if (fd && !fd->isExtern && !discarded)
    {
        NoteFreshOwned(b, result);
    }

    return result;
}

// A call to a Strata or extern function: lower the arguments, add the hidden
// trailing ones, resolve the callee, make the call, then shape the result.
static Value EmitFunctionCall(Builder* b, CallExpr* n, FuncInfo* info, bool discarded)
{
    const FunctionDecl* fd = n->resolvedDecl;

    // A typed rest param collects the trailing call args into one T[] array
    // passed in the rest param's slot, so the LLVM call has exactly
    // `params.count` args. A bare extern `...` passes every arg straight
    // through as a real C vararg call. A `return` param is synthesized by
    // the caller as one out-pointer slot, so the LLVM call also has exactly
    // `params.count` args (one more than the user wrote).
    bool hasReturnParam = fd && FunctionHasReturnParam(fd);
    bool typedRest = fd && fd->isVariadic && !fd->isCVararg && fd->params.count > 0;

    size_t nargs = (typedRest || hasReturnParam) ? fd->params.count : n->args.count;
    LLVMValueRef* args = NULL;

    if (nargs > 0)
    {
        args = (LLVMValueRef*)ArenaAlloc(b->arena, nargs * sizeof(LLVMValueRef));
    }

    LLVMValueRef returnParamSlot = NULL;
    TypeDesc returnParamTd = {0};

    // Array literals lent to `ref T[]` params: caller-owned temps, dropped
    // after the call.
    Vec ownedArgTemps;
    VecInit(&ownedArgTemps);

    EmitCallArgs(b, n, info, fd, args, nargs, &ownedArgTemps, &returnParamSlot, &returnParamTd);

    unsigned finalCount = (unsigned)nargs;
    LLVMValueRef* finalArgs = PrependHiddenCtxArg(b, info, args, &finalCount);
    finalArgs = AppendTypeDescArg(b, info, fd, finalArgs, &finalCount);

    LLVMValueRef callee = info->function;
    bool slotExtern = false;

    if (n->delegateCallee)
    {
        callee = EmitDelegateCallee(b, n);
    }

    callee = ResolveJitExternCallee(b, n, callee, &slotExtern);

    LLVMValueRef call = EmitCallInstruction(b, n, info, callee, finalArgs, finalCount, slotExtern);

    DropOwnedArgTemps(b, &ownedArgTemps);

    call = NarrowExternIntReturn(b, fd, info, call);

    return EmitCallResult(b, fd, info, call, discarded, hasReturnParam, returnParamSlot, returnParamTd);
}

Value EmitCall(Builder* b, CallExpr* n)
{
    // A direct expression-statement call has an unused result. Consumed here
    // (cleared before any nested call) so only the outermost call sees it.
    bool discarded = b->discardCallResult;
    b->discardCallResult = false;

    if (n->isIntrinsicCall)
    {
        bool isValid = 0;
        Value result = EmitIntrinsicCall(b, n, &isValid);

        if (isValid)
        {
            return result;
        }
    }

    if (StrMapGet(&b->structTypes, n->callee) != NULL)
    {
        return EmitStructInitCall(b, n);
    }

    FuncInfo* info = (FuncInfo*)StrMapGet(&b->funcs, n->callee);

    if (!info)
    {
        if (b->diag)
        {
            DiagErrorFmt(b->diag, n->base.range, "call to unknown function '%s'", n->callee);
        }

        return ZeroInt(b);
    }

    return EmitFunctionCall(b, n, info, discarded);
}
