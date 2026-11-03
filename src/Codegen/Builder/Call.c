/* Builder: Calls: function lookup, arguments, builtins, intrinsics, extern slots. */
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

/* Statically names the handle type of `n` WITHOUT emitting IR (used to detect
   impl property access before the regular lvalue paths). NULL when unknown. */
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
        Value* sym = (Value*)StrMapGet(&b->m_symbols, id->name);

        if (!sym)
        {
            sym = (Value*)StrMapGet(&b->m_globals, id->name);
        }

        if (!sym)
        {
            return NULL;
        }

        /* Names a struct or handle (both carry structTypeName); chains and
           property lookup branch on opacity downstream. */
        if (sym->typeDesc.structTypeName)
        {
            return sym->typeDesc.structTypeName;
        }

        if ((TypeDescIsBox(&sym->typeDesc) || TypeDescIsOptional(&sym->typeDesc)) && sym->typeDesc.boxInner
            && TypeRegistryIsOpaque(&b->m_registry, sym->typeDesc.boxInner->name))
        {
            return sym->typeDesc.boxInner->name; /* `T?` receiver */
        }

        return NULL;
    }
    case NodeCall:
    {
        const FunctionDecl* fd = ((CallExpr*)n)->resolvedDecl;

        return (fd && TypeRegistryIsOpaque(&b->m_registry, fd->returnType.name)) ? fd->returnType.name : NULL;
    }
    case NodeCast:
    {
        const TypeName* t = &((CastExpr*)n)->type;

        return TypeRegistryIsOpaque(&b->m_registry, t->name) ? t->name : NULL;
    }
    case NodeNullTest:
        return StaticExprTypeName(b, ((NullTestExpr*)n)->operand); /* `T?` -> T */
    case NodeIndex:
    {
        /* `cams[i].Prop` — the element type comes from the base's TypeDesc
           (arrayInner points into the declaration's stable TypeName tree). */
        IndexExpr* ix = (IndexExpr*)n;
        Node* baseNode = ix->base_node;
        const TypeName* inner = NULL;

        if (baseNode->kind == NodeIdent)
        {
            IdentExpr* id = (IdentExpr*)baseNode;
            Value* sym = (Value*)StrMapGet(&b->m_symbols, id->name);

            if (!sym)
            {
                sym = (Value*)StrMapGet(&b->m_globals, id->name);
            }

            inner = sym ? sym->typeDesc.arrayInner : NULL;
        }
        else if (baseNode->kind == NodeMember)
        {
            MemberExpr* bm = (MemberExpr*)baseNode;
            const char* bn = StaticExprTypeName(b, bm->base_node);

            if (bn && !TypeRegistryIsOpaque(&b->m_registry, bn))
            {
                int fidx = TypeRegistryFieldIndex(&b->m_registry, bn, bm->member);

                if (fidx >= 0)
                {
                    const StructType* st = TypeRegistryFind(&b->m_registry, bn);
                    FieldDecl* field = (FieldDecl*)VecGet((Vec*)&st->fields, (size_t)fidx);
                    inner = TypeNameHasArrayShape(&field->type) ? field->type.elem : NULL;
                }
            }
        }

        return (inner && TypeRegistryIsOpaque(&b->m_registry, inner->name)) ? inner->name : NULL;
    }
    case NodeMember:
    {
        MemberExpr* m = (MemberExpr*)n;
        const char* baseName = StaticExprTypeName(b, m->base_node);

        if (!baseName)
        {
            return NULL;
        }

        if (TypeRegistryIsOpaque(&b->m_registry, baseName))
        {
            /* Member of a handle: only an impl property continues the chain. */
            ImplPropEntry* e
                = (ImplPropEntry*)StrMapGet(&b->m_implProps, arena_format(b->m_arena, "%s.%s", baseName, m->member));

            return (e && TypeRegistryIsOpaque(&b->m_registry, e->decl->returnType.name)) ? e->decl->returnType.name
                                                                                         : NULL;
        }

        int idx = TypeRegistryFieldIndex(&b->m_registry, baseName, m->member);

        if (idx < 0)
        {
            return NULL;
        }

        const StructType* st = TypeRegistryFind(&b->m_registry, baseName);
        FieldDecl* field = (FieldDecl*)VecGet((Vec*)&st->fields, (size_t)idx);
        const TypeName* inner
            = (TypeNameIsBox(&field->type) || TypeNameIsOptional(&field->type)) ? field->type.inner : &field->type;

        return (inner && TypeRegistryIsOpaque(&b->m_registry, inner->name)) ? inner->name : NULL;
    }
    default:
        return NULL;
    }
}

/* The JIT's `__strata_ext_<name>` slot for an extern, created on first use: every `extern<T>`
   instantiation shares the generic's slot, since the host binds one function for all T. */
LLVMValueRef ExternSlot(Builder* b, const char* name)
{
    LLVMValueRef slot = (LLVMValueRef)StrMapGet(&b->m_externSlots, name);

    if (!slot)
    {
        slot = LLVMAddGlobal(b->m_mod, b->m_ptrTy, arena_format(b->m_arena, "__strata_ext_%s", name));
        LLVMSetInitializer(slot, LLVMConstNull(b->m_ptrTy));
        StrMapPut(&b->m_externSlots, name, (void*)slot);
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

    /* A piece of a temporary (`f(mkNested()[1])`): pass its storage inside
       the temporary, like an lvalue, so a callee that takes ownership nulls
       it there and the temporary's drop does not free it again. */
    const TempPart* tp = FindTempPart(b, MovableBoxSourceNode(arg));

    if (tp)
    {
        return tp->addr;
    }

    LLVMValueRef slot = EntryAlloca(b, value.typeDesc.type, "outarg");
    LLVMBuildStore(b->m_builder, value.value, slot);

    return slot;
}

/* Applies C default argument promotions to a value passed through a bare
   extern varargs */
static LLVMValueRef ApplyCVarargPromotion(Builder* b, Value v)
{
    /* A string vararg crosses as char* (printf("%s", s) et al). */
    if (v.typeDesc.isString)
    {
        return BuildExternStringPtr(b, v);
    }

    LLVMTypeRef ty = v.typeDesc.type;

    if (ty == I1Ty(b))
    {
        return LLVMBuildZExt(b->m_builder, v.value, I32Ty(b), "vap");
    }

    if (ty == LLVMInt8TypeInContext(b->m_ctx) || ty == LLVMInt16TypeInContext(b->m_ctx))
    {
        return v.typeDesc.isUnsigned ? LLVMBuildZExt(b->m_builder, v.value, I32Ty(b), "vap")
                                     : LLVMBuildSExt(b->m_builder, v.value, I32Ty(b), "vap");
    }

    if (ty == LLVMFloatTypeInContext(b->m_ctx))
    {
        return LLVMBuildFPExt(b->m_builder, v.value, LLVMDoubleTypeInContext(b->m_ctx), "vap");
    }

    return v.value;
}

/* addressof(x): x's storage (a box's cell), or an extern / `@export` function's entry point. */
static Value EmitAddressofBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc ptrTd = ResolveByName(b, STRATA_PTR_TYPE);
    const FunctionDecl* fn = n->addressOfFunction;

    if (fn)
    {
        /* A JIT extern is reached through its slot, which the host binds. */
        if (b->m_jitMode && fn->isExtern)
        {
            return ValueMake(LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ExternSlot(b, fn->name), "fnaddr"), ptrTd);
        }

        FuncInfo* info = (FuncInfo*)StrMapGet(&b->m_funcs, fn->mangledName);

        return ValueMake(info ? info->function : LLVMConstNull(b->m_ptrTy), ptrTd);
    }

    Node* arg = (Node*)VecGet(&n->args, 0);

    while (arg->kind == NodeCast)
    {
        arg = ((CastExpr*)arg)->operand;
    }

    LValue lv = EmitLValue(b, arg);

    if (!lv.valid || lv.vectorLane)
    {
        if (b->m_diag)
        {
            DiagError(b->m_diag, arg->range, "cannot take the address of this expression");
        }

        return ValueMake(LLVMConstNull(b->m_ptrTy), ptrTd);
    }

    LLVMValueRef addr = lv.ptr;

    if (TypeDescIsBox(&lv.typeDesc))
    {
        addr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "cell");
    }

    return ValueMake(addr, ptrTd);
}

/* sizeof<T>() / alignof<T>(): T's C size and alignment, as ulong constants. */
static Value EmitSizeofBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    LLVMTypeRef ty = TypeDescHasArrayShape(&td) ? ArrayStructType(b) : td.type;
    TypeDesc ulongTd = TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL);

    if (strcmp(n->callee, "alignof") == 0)
    {
        /* offsetof({ i8, T }, 1) */
        LLVMTypeRef members[2] = {LLVMInt8TypeInContext(b->m_ctx), ty};
        LLVMTypeRef probe = LLVMStructTypeInContext(b->m_ctx, members, 2, 0);
        LLVMValueRef idx[2] = {LLVMConstInt(I32Ty(b), 0, 0), LLVMConstInt(I32Ty(b), 1, 0)};
        LLVMValueRef gep = LLVMConstGEP2(probe, LLVMConstNull(b->m_ptrTy), idx, 2);

        return ValueMake(LLVMConstPtrToInt(gep, I64Ty(b)), ulongTd);
    }

    return ValueMake(SizeOfConst(b, ty), ulongTd);
}

/* load<T>(p): a T read from raw memory (no alignment assumed). */
static Value EmitLoadBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    Value p = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, td.type, p.value, "load");
    LLVMSetAlignment(v, 1);

    return ValueMake(v, td);
}

/* store(p, v): v written to raw memory as the call's T (no alignment assumed). */
static Value EmitStoreBuiltin(Builder* b, CallExpr* n)
{
    TypeDesc td = Resolve(b, n->typeArg);
    Value p = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value v = Coerce(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)), td);
    LLVMValueRef st = LLVMBuildStore(b->m_builder, v.value, p.value);
    LLVMSetAlignment(st, 1);

    return ValueMake(NULL, TypeDescMake(LLVMVoidTypeInContext(b->m_ctx), TD_VOID, NULL));
}

static Value EmitIntrinsicCall(Builder* b, CallExpr* n, bool* isValid)
{
    static const IntrinsicDefinition intrinsics[] = {
        /* Array calls */
        {"array_push",        EmitArrayBuiltin    },
        {"array_pop",         EmitArrayBuiltin    },
        {"array_resize",      EmitArrayBuiltin    },
        {"array_remove",      EmitArrayBuiltin    },
        {"array_reserve",     EmitArrayBuiltin    },
        {"array_swap_remove", EmitArrayBuiltin    },
        {"array_replace",     EmitArrayBuiltin    },
        {"array_take",        EmitArrayBuiltin    },

        /* Misc */
        {"copy",              EmitCopyBuiltin     },
        {"drop",              EmitDropBuiltin     },

        /* Raw memory */
        {"addressof",         EmitAddressofBuiltin},
        {"sizeof",            EmitSizeofBuiltin   },
        {"alignof",           EmitSizeofBuiltin   },
        {"load",              EmitLoadBuiltin     },
        {"store",             EmitStoreBuiltin    },

        /* Strings */
        {"substring",         EmitSubstringBuiltin},

        /* Vectors */
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

Value EmitCall(Builder* b, CallExpr* n)
{
    /* A direct expression-statement call has an unused result. Consumed here
       (cleared before any nested call) so only the outermost call sees it. */
    bool discarded = b->m_discardCallResult;
    b->m_discardCallResult = false;

    if (n->isIntrinsicCall)
    {
        bool isValid = 0;
        Value result = EmitIntrinsicCall(b, n, &isValid);

        if (isValid)
        {
            return result;
        }
    }

    // Is it a struct initializer call?
    if (StrMapGet(&b->m_structTypes, n->callee) != NULL)
    {
        TypeName tn = MakeTypeName(b, n->callee);
        TypeDesc typeDesc = Resolve(b, &tn);

        const StructType* st = TypeRegistryFind(&b->m_registry, n->callee);

        /* Fields the call leaves out keep their declared defaults, as in a struct literal. */
        LLVMValueRef agg = DefaultValueOf(b, &tn, typeDesc);

        size_t nargs = n->args.count;

        for (size_t i = 0; i < nargs && st; i++)
        {
            if (i >= st->fields.count)
            {
                break;
            }

            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, i);
            TypeDesc fieldTd = Resolve(b, &fieldDecl->type);

            Node* argNode = (Node*)VecGet(&n->args, i);
            Value rawArg = EmitExpr(b, argNode);
            Value argValue;

            /* A string field takes a heap copy of a literal so the field can
               be freed without freeing a (borrowed) string constant. */
            if (fieldTd.isString && argNode->kind == NodeStrLiteral)
            {
                LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, rawArg.value, 0, "fslit.p");
                LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, rawArg.value, 1, "fslit.l");
                argValue = ValueMake(BuildOwnedStringFat(b, sp, sl), fieldTd);
            }
            else if (TypeDescIsBox(&fieldTd) && fieldTd.boxInner
                     && !(TypeDescIsBox(&rawArg.typeDesc) && rawArg.typeDesc.boxInner
                          && strcmp(rawArg.typeDesc.boxInner->name, fieldTd.boxInner->name) == 0))
            {
                /* ^T / T? field from a non-box value (bare T, braced struct
                   literal): allocate a T slot and construct the value into it.
                   A braced `{}` against a `T?` field constructs a NON-EMPTY
                   boxed T (same rule as the named-literal form). */
                LLVMValueRef heap = EmitBoxCell(b, fieldTd.boxInner, rawArg, argNode, "argbox");
                argValue = ValueMake(heap, fieldTd);
            }
            else
            {
                argValue = Coerce(b, rawArg, fieldTd);
            }

            agg = InsertStructField(b, st, agg, (int)i, argValue.value);

            /* If an owning field was moved from an owning lvalue source,
               null the source so its scope-exit drop is a no-op. */
            if (FieldInitMovesSource(b, fieldTd, rawArg.typeDesc, argNode))
            {
                NullMovedSource(b, argNode);
            }
        }

        return ValueMake(agg, typeDesc);
    }

    FuncInfo* info = (FuncInfo*)StrMapGet(&b->m_funcs, n->callee);

    if (!info)
    {
        if (b->m_diag)
        {
            DiagErrorFmt(b->m_diag, n->base.range, "call to unknown function '%s'", n->callee);
        }

        return ZeroInt(b);
    }

    const FunctionDecl* fd = n->resolvedDecl;

    /* A typed rest param collects the trailing call args into one T[] array
       passed in the rest param's slot, so the LLVM call has exactly
       `params.count` args. A bare extern `...` passes every arg straight
       through as a real C vararg call. A `return` param is synthesized by
       the caller as one out-pointer slot, so the LLVM call also has exactly
       `params.count` args (one more than the user wrote). */
    bool hasReturnParam = fd && FunctionHasReturnParam(fd);
    bool typedRest = fd && fd->isVariadic && !fd->isCVararg && fd->params.count > 0;
    bool cVararg = fd && fd->isCVararg;

    size_t nargs = (typedRest || hasReturnParam) ? fd->params.count : n->args.count;
    LLVMValueRef* args = NULL;

    if (nargs > 0)
    {
        args = (LLVMValueRef*)arena_alloc(b->m_arena, nargs * sizeof(LLVMValueRef));
    }

    LLVMValueRef returnParamSlot = NULL;
    TypeDesc returnParamTd = {0};

    /* Array literals lent to `ref T[]` params: caller-owned temps, dropped
       after the call. */
    Vec ownedArgTemps;
    VecInit(&ownedArgTemps);

    for (size_t k = 0; k < nargs; k++)
    {
        /* Return-param slot: a fresh temp the callee writes its result into;
           loaded back into a value after the call. */
        if (hasReturnParam && k == fd->params.count - 1)
        {
            returnParamTd = Resolve(b, &fd->returnType);
            returnParamSlot = EntryAlloca(b, returnParamTd.type, "retparam");
            args[k] = returnParamSlot;
            continue;
        }

        /* Typed rest slot: gather the trailing call args into a T[]. The
           buffer is stack-allocated; a `ref` rest borrows owning elements
           instead of moving them. */
        if (typedRest && k == fd->params.count - 1)
        {
            const ParamDecl* restParam = (ParamDecl*)VecGet(&fd->params, fd->params.count - 1);
            const TypeName* elemType = TypeNameArrayElem(&restParam->type);

            if (!elemType)
            {
                TypeName empty = MakeTypeName(b, "");
                elemType = (const TypeName*)arena_dup(b->m_arena, &empty, sizeof(TypeName));
            }

            Vec tail;
            VecInit(&tail);

            for (size_t i = fd->params.count - 1; i < n->args.count; i++)
            {
                VecPush(&tail, VecGet(&n->args, i));
            }

            Value arr = EmitArrayFromNodes(b, elemType, &tail, true, restParam->mod == ModRef);

            LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "rest");
            LLVMBuildStore(b->m_builder, arr.value, slot);
            args[k] = slot;
            continue;
        }

        bool shouldPassByPtr = k < info->paramByPtrCount && info->paramByPtr[k];
        Node* argNode = (Node*)VecGet(&n->args, k);

        /* ^T coerced to T: if the param is a plain struct (not box),
           and the arg is a box, the heap pointer IS the T* the param wants. */
        bool paramIsBoxType
            = fd && k < fd->params.count && BuilderIsOwningType(b, &((ParamDecl*)VecGet(&fd->params, k))->type);

        bool argIsBox = false;

        if (shouldPassByPtr && !paramIsBoxType)
        {
            if (argNode->kind == NodeIdent)
            {
                Value* sym = (Value*)StrMapGet(&b->m_symbols, ((IdentExpr*)argNode)->name);

                if (!sym)
                {
                    sym = (Value*)StrMapGet(&b->m_globals, ((IdentExpr*)argNode)->name);
                }

                argIsBox = sym && TypeDescIsBox(&sym->typeDesc);
            }
            else if (argNode->kind == NodeCall)
            {
                FuncInfo* fi = (FuncInfo*)StrMapGet(&b->m_funcs, ((CallExpr*)argNode)->callee);
                argIsBox = fi && TypeDescIsBox(&fi->returnType);
            }
        }

        /* `extern` array decay - T[] in extern functions are punned to T* */
        if (fd && fd->isExtern && k < fd->params.count)
        {
            const ParamDecl* pd = (ParamDecl*)VecGet(&fd->params, k);

            if (pd->mod == ModNone)
            {
                const TypeName* pty = &pd->type;
                const TypeName* elem = TypeNameArrayElem(pty);

                if (TypeNameIsDynamicArray(pty) && !(elem && BuilderIsOwningValue(b, elem)))
                {
                    LLVMValueRef arrAddr = ArgAddress(b, argNode);
                    LLVMValueRef dataSlot = ArrayDataPtr(b, arrAddr);
                    args[k] = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataSlot, "arrptr");
                    continue;
                }
            }
        }

        /* Array literal against a dynamic-array param: the param's shape
           stamps the element types (sema), so the literal can construct. */
        if (shouldPassByPtr && fd && k < fd->params.count && argNode->kind == NodeArrayInit)
        {
            const ParamDecl* litParam = (ParamDecl*)VecGet(&fd->params, k);

            if (TypeNameIsDynamicArray(&litParam->type))
            {
                ArrayInitExpr* ai = AsNode(ArrayInitExpr, argNode);
                Value arr;

                if (litParam->mod == ModRef)
                {
                    /* `ref T[]` borrows: construct the literal on the STACK
                       (a la the typed rest slot). The caller drops owning
                       elements after the call; the stack buffer is never
                       freed. */
                    arr = EmitArrayFromNodes(b, ai->elementType, &ai->elements, true, false);
                }
                else
                {
                    /* By value: a normal heap-backed dynamic array — the
                       callee owns it and drops it at exit. */
                    arr = EmitArrayInit(b, ai);
                }

                LLVMValueRef litSlot = EntryAlloca(b, ArrayStructType(b), "litarr");
                LLVMBuildStore(b->m_builder, arr.value, litSlot);
                args[k] = litSlot;

                if (litParam->mod == ModRef)
                {
                    OwnedArgTemp* temp = (OwnedArgTemp*)arena_alloc(b->m_arena, sizeof(OwnedArgTemp));
                    temp->slot = litSlot;
                    temp->td = arr.typeDesc;
                    temp->stackOnly = true;
                    VecPush(&ownedArgTemps, temp);
                }

                continue;
            }
        }

        /* Fixed array → `ref T[]` param: borrow the inline storage as a
           stack fat {&fixed[0], N, N} — a slice/view with no allocation and
           no copy (same stack-fat trick as the typed rest slot). The callee
           reads/writes elements through the data pointer; a `ref` binding is
           never dropped or freed (ref params are exempt from the
           owning-local teardown). */
        if (shouldPassByPtr && fd && k < fd->params.count
            && (argNode->kind == NodeIdent || argNode->kind == NodeMember))
        {
            const ParamDecl* viewParam = (ParamDecl*)VecGet(&fd->params, k);

            if (viewParam->mod == ModRef && TypeNameIsDynamicArray(&viewParam->type))
            {
                LValue lv = EmitLValue(b, argNode);

                if (lv.valid && lv.typeDesc.isFixedArray)
                {
                    LLVMValueRef zero[2] = {LLVMConstInt(I64Ty(b), 0, 0), LLVMConstInt(I64Ty(b), 0, 0)};
                    LLVMValueRef data = LLVMBuildGEP2(b->m_builder, lv.typeDesc.type, lv.ptr, zero, 2, "fixview");
                    LLVMValueRef len = LLVMConstInt(I32Ty(b), (unsigned long long)lv.typeDesc.fixedLength, 0);
                    LLVMValueRef fat = LLVMGetUndef(ArrayStructType(b));
                    fat = LLVMBuildInsertValue(b->m_builder, fat, data, 0, "fixview.p");
                    fat = LLVMBuildInsertValue(b->m_builder, fat, len, 1, "fixview.l");
                    fat = LLVMBuildInsertValue(b->m_builder, fat, len, 2, "fixview.c");

                    LLVMValueRef viewSlot = EntryAlloca(b, ArrayStructType(b), "fixview");
                    LLVMBuildStore(b->m_builder, fat, viewSlot);
                    args[k] = viewSlot;
                    continue;
                }
            }
        }

        if (shouldPassByPtr && paramIsBoxType && fd && k < fd->params.count && argNode->kind != NodeStrLiteral
            && TypeIsString(&b->m_registry, ((ParamDecl*)VecGet((Vec*)&fd->params, k))->type.name))
        {
            /* Internal string param (by slot address): the callee owns the
               fat and drops/nulls the slot at exit. Dispatch on the EMITTED
               value's type, never the arg's AST shape: a `cstring` of ANY
               form (element, member chain, call result, cast) is borrowed
               bytes that must be heap-copied into a fresh owned fat. */
            LValue lv = EmitLValue(b, argNode);
            /* A `ref string` param only borrows: owned temporaries made for it die with the statement. */
            const ParamDecl* strParam = (const ParamDecl*)VecGet((Vec*)&fd->params, k);
            bool borrowsString = strParam->mod == ModRef;

            if (lv.valid && !lv.vectorLane && !lv.typeDesc.isCString)
            {
                /* A string lvalue: pass the caller's own storage (move
                   semantics — the callee nulls the slot at exit). */
                args[k] = lv.ptr;
            }
            else
            {
                Value av = lv.valid
                               ? ValueMake(LLVMBuildLoad2(b->m_builder, lv.typeDesc.type, lv.ptr, "csarg"), lv.typeDesc)
                               : EmitExpr(b, argNode);

                if (av.typeDesc.isCString)
                {
                    LLVMValueRef owned = BuildOwnedStringFromCStr(b, av.value);

                    if (borrowsString)
                    {
                        args[k] = ParkTemp(b, ValueMake(owned, Resolve(b, &strParam->type)));
                    }
                    else
                    {
                        LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "cs2s");
                        LLVMBuildStore(b->m_builder, owned, slot);

                        args[k] = slot;
                    }
                }
                else
                {
                    const TempPart* tp = FindTempPart(b, MovableBoxSourceNode(argNode));

                    if (tp)
                    {
                        /* A string piece of a temporary (`f(mkArr()[0])`):
                           hand over its storage inside the temporary, like
                           an lvalue, so the callee's null-at-exit keeps the
                           temporary's drop from freeing it again. */
                        args[k] = tp->addr;
                    }
                    else if (borrowsString)
                    {
                        args[k] = ParkTemp(b, av);
                    }
                    else
                    {
                        /* A non-lvalue string value (e.g. a call result):
                           spill the already-computed fat (ArgAddress's
                           fallback, minus a second evaluation). */
                        LLVMValueRef slot = EntryAlloca(b, av.typeDesc.type, "outarg");
                        LLVMBuildStore(b->m_builder, av.value, slot);

                        args[k] = slot;
                    }
                }
            }
        }
        else if (shouldPassByPtr && paramIsBoxType && argNode->kind == NodeStrLiteral)
        {
            /* Internal string param (by slot address): the callee owns and
               drops the value, so materialize an owned fat into a fresh
               temp slot instead of handing out the borrowed constant. */
            Value litVal = EmitExpr(b, argNode);
            const ParamDecl* litParam
                = fd && k < fd->params.count ? (const ParamDecl*)VecGet((Vec*)&fd->params, k) : NULL;
            LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "stslot");

            if (litParam && litParam->mod == ModRef)
            {
                /* A `ref string` param only reads: lend it the constant itself. */
                LLVMBuildStore(b->m_builder, litVal.value, slot);
            }
            else
            {
                LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, litVal.value, 0, "arglit.p");
                LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, litVal.value, 1, "arglit.l");
                LLVMBuildStore(b->m_builder, BuildOwnedStringFat(b, sp, sl), slot);
            }

            args[k] = slot;
        }
        else if (shouldPassByPtr && !paramIsBoxType && argIsBox)
        {
            /* The callee borrows the box's contents. A box that is only a temporary (`F(Box(1))`) has no other
               owner: park it so the end of the statement drops it. */
            Value boxArg = EmitExpr(b, argNode);

            if (argNode->kind != NodeIdent && argNode->kind != NodeMember && argNode->kind != NodeIndex
                && TypeDescIsOwning(b, &boxArg.typeDesc))
            {
                ParkTemp(b, boxArg);
            }

            args[k] = boxArg.value;
        }
        else if (!shouldPassByPtr && paramIsBoxType && fd && fd->isExtern && k < fd->params.count)
        {
            /* Extern ^T / T? param: pass the pointer ITSELF by value.
               A box/optional arg already is that pointer; a plain owning
               value is constructed into a fresh cell first. */
            const ParamDecl* extParam = (ParamDecl*)VecGet(&fd->params, k);
            const TypeName* extTy = &extParam->type;

            Value av = EmitExpr(b, argNode);

            /* Extern ^T param + array arg: array decay - the T[] / T[N]
               argument is passed as a pointer to its first element (the box
               IS that pointer). Dynamic arrays hand over their data buffer;
               fixed arrays GEP to element 0. */
            if (TypeNameIsBox(extTy) && (TypeDescHasArrayShape(&av.typeDesc) || av.typeDesc.isFixedArray))
            {
                const TypeName* boxInner = TypeNameBoxInner(extTy);
                const TypeName* argElem = av.typeDesc.arrayInner;

                if (boxInner && argElem && strcmp(boxInner->name, argElem->name) == 0)
                {
                    if (TypeDescHasArrayShape(&av.typeDesc))
                    {
                        args[k] = LLVMBuildExtractValue(b->m_builder, av.value, 0, "arrptr");
                    }
                    else
                    {
                        LLVMValueRef slot = ArgAddress(b, argNode);
                        LLVMValueRef zero[1] = {LLVMConstInt(I64Ty(b), 0, 0)};
                        args[k] = LLVMBuildGEP2(b->m_builder, av.typeDesc.type, slot, zero, 1, "farr");
                    }

                    continue;
                }
            }

            bool paramIsRealBox = TypeNameIsBox(extTy) || TypeNameIsOptional(extTy);

            if (av.typeDesc.isString)
            {
                if (paramIsRealBox)
                {
                    /* `string?` param: pass the RAW data pointer — NULL stays
                       NULL (empty optional), preserving the C-side NULL check. */
                    args[k] = LLVMBuildExtractValue(b->m_builder, av.value, 0, "optstr");
                }
                else
                {
                    /* Extern string param: pun the fat to char*
                       (NUL-terminated; empty {null, 0} materializes a valid
                       empty C string). */
                    ClaimTemp(b, av); /* C only borrows: an owned temporary dies with the statement */
                    args[k] = BuildExternStringPtr(b, av);
                }
            }
            else if (TypeDescIsBox(&av.typeDesc))
            {
                args[k] = paramIsRealBox ? av.value : DerefBoxValue(b, av).value;
            }
            else
            {
                const TypeName* innerTn = TypeNameBoxInner(extTy) ? TypeNameBoxInner(extTy) : StringTypeName(b);

                if (!paramIsRealBox)
                {
                    /* Plain `string` param: the value already is the char*. */
                    args[k] = av.value;
                }
                else
                {
                    LLVMValueRef owned = EmitOwnedValue(b, av, argNode, innerTn);

                    LLVMValueRef cellSz = SizeOfConst(b, b->m_ptrTy);
                    LLVMValueRef cellArgs[1] = {cellSz};
                    StrataAllocFn(b);
                    LLVMValueRef cell
                        = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, cellArgs, 1, "argcell");
                    LLVMBuildStore(b->m_builder, owned, cell);

                    args[k] = cell;
                }
            }
        }
        else
        {
            /* A box arg passed to a by-value (non-indirect) param - e.g. a
               plain handle - must be dereferenced to its value, not passed
               as the box's own heap pointer. */
            Value v = EmitExpr(b, argNode);

            if (cVararg && k >= fd->params.count)
            {
                /* Bare extern `...` trailing args follow C default promotions. */
                /* C only borrows: an owned temporary (`printf("%s", mk())`) dies with the statement. */
                ClaimTemp(b, v);
                args[k] = ApplyCVarargPromotion(b, DerefBoxValue(b, v));
            }
            else
            {
                const ParamDecl* refParam
                    = fd && k < fd->params.count ? (const ParamDecl*)VecGet((Vec*)&fd->params, k) : NULL;
                bool borrowsTemp = shouldPassByPtr && refParam && refParam->mod == ModRef && !fd->isExtern
                                   && argNode->kind != NodeIdent && argNode->kind != NodeMember
                                   && argNode->kind != NodeIndex && TypeDescIsOwning(b, &v.typeDesc);

                if (borrowsTemp)
                {
                    /* An owned temporary lent to a `ref` param: dropped when the statement ends. */
                    args[k] = ParkTemp(b, v);
                }
                else if (shouldPassByPtr && argNode->kind == NodeCall)
                {
                    /* A call result has no address: spill the value already
                       computed above. ArgAddress would evaluate the call a
                       second time (repeating its side effects and leaking the
                       first result). */
                    LLVMValueRef slot = EntryAlloca(b, v.typeDesc.type, "outarg");
                    LLVMBuildStore(b->m_builder, v.value, slot);
                    args[k] = slot;
                }
                else if (shouldPassByPtr)
                {
                    {
                        /* A box-typed field or element (`h.buffer.Method()`, `ref T` param from a `^T`
                           / `T?` slot): the callee wants the boxed value, not the slot. */
                        LValue slot = {0};

                        if (!paramIsBoxType && !argIsBox && (argNode->kind == NodeMember || argNode->kind == NodeIndex))
                        {
                            slot = EmitLValue(b, argNode);
                        }

                        if (slot.valid && !slot.vectorLane && TypeDescIsBox(&slot.typeDesc))
                        {
                            args[k] = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slot.ptr, "boxarg");
                        }
                        else if (slot.valid && !slot.vectorLane)
                        {
                            args[k] = slot.ptr;
                        }
                        else
                        {
                            args[k] = ArgAddress(b, argNode);
                        }
                    }
                }
                else
                {
                    v = DerefBoxValue(b, v);

                    if (fd && k < fd->params.count)
                    {
                        /* Implicit scalar widening/narrowing to the declared
                           param type (e.g. a float literal to a double param)
                           - without this the arg crosses at its own width and
                           the call gets a mismatched ABI (garbage). */
                        TypeDesc paramTd = Resolve(b, &((ParamDecl*)VecGet(&fd->params, k))->type);
                        v = Coerce(b, v, paramTd);

                        /* An owning struct passed by value: the callee owns
                           and drops its copy at return, so hand it a deep
                           copy — the caller's value keeps its own fields
                           (C value semantics; `ref` is the zero-copy escape). */
                        if (paramTd.structTypeName
                            && TypeRegistryIsOwningStruct(&b->m_registry, paramTd.structTypeName))
                        {
                            v = EmitCopyValue(b, v, paramTd);
                        }
                    }

                    args[k] = v.value;
                }
            }
        }
    }

    /* The callee's own hidden context pointer (see DeclareFunction) is never
       written by Strata source; forward the CALLING function's pointer
       automatically. `impl` self-insertion already happened at the sema
       level (an ordinary arg inside [0,nargs)), so this composes for free:
       final order is [ctx, self, ...rest]. */
    LLVMValueRef* finalArgs = args;
    unsigned finalCount = (unsigned)nargs;

    if (info->hasHiddenCtxParam)
    {
        finalArgs = (LLVMValueRef*)arena_alloc(b->m_arena, (nargs + 1) * sizeof(LLVMValueRef));
        finalArgs[0] = b->m_curGlobalsPtr;

        if (nargs > 0)
        {
            memcpy(finalArgs + 1, args, nargs * sizeof(LLVMValueRef));
        }

        finalCount = (unsigned)(nargs + 1);
    }

    /* `extern<T>`: T's descriptor goes last, after every argument the call spells out. */
    if (info->hasTypeDescParam && fd && fd->typeArg)
    {
        LLVMValueRef* withTypeDesc = (LLVMValueRef*)arena_alloc(b->m_arena, (finalCount + 1) * sizeof(LLVMValueRef));

        if (finalCount > 0)
        {
            memcpy(withTypeDesc, finalArgs, finalCount * sizeof(LLVMValueRef));
        }

        withTypeDesc[finalCount] = TypeDescConstant(b, fd->typeArg);
        finalArgs = withTypeDesc;
        finalCount++;
    }

    LLVMValueRef callee = info->function;
    bool slotExtern = false;

    /* Through a delegate: the pointer value is the callee; a null one panics instead of jumping to 0. */
    if (n->delegateCallee)
    {
        callee = EmitExpr(b, n->delegateCallee).value;

        LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, callee, LLVMConstNull(b->m_ptrTy), "dlgnull");
        LLVMBasicBlockRef nullBB = NewBb(b, "dlg.null");
        LLVMBasicBlockRef callBB = NewBb(b, "dlg.call");
        LLVMBuildCondBr(b->m_builder, isNull, nullBB, callBB);

        PositionAtEnd(b, nullBB);
        LLVMValueRef panicArgs[1] = {MsgGlobalPtr(
            b, ".pmsg", arena_format(b->m_arena, "call through a null '%s' delegate", n->delegateDecl->name))};
        StrataPanicFn(b);
        LLVMBuildCall2(b->m_builder, b->m_panicFnType, b->m_panicFn, panicArgs, 1, "");
        LLVMBuildUnreachable(b->m_builder);

        PositionAtEnd(b, callBB);
    }

    if (b->m_jitMode)
    {
        LLVMValueRef slot = (LLVMValueRef)StrMapGet(&b->m_externSlots, n->callee);

        if (slot)
        {
            slotExtern = true;
            LLVMValueRef fnPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slot, "extfn");
            callee = fnPtr;
        }
    }

    LLVMValueRef call;

    if (slotExtern && b->m_nullExternCheck)
    {
        /* JIT extern slots start null; a call before strataJitAddSymbol bound
           the host fn would jump to address 0. Under nullExternCall, panic with
           the unbound name. The panic block RETURNS a zero value so a host
           panic handler that returns keeps the JIT frame's stack walker away. */
        LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, callee, LLVMConstNull(b->m_ptrTy), "extnull");
        LLVMBasicBlockRef nullBB = NewBb(b, "ext.null");
        LLVMBasicBlockRef callBB = NewBb(b, "ext.call");
        LLVMBuildCondBr(b->m_builder, isNull, nullBB, callBB);
        b->m_terminated = true;

        PositionAtEnd(b, nullBB);
        {
            LLVMValueRef args2[1]
                = {MsgGlobalPtr(b, ".pmsg", arena_format(b->m_arena, "call to null extern function '%s'", n->callee))};
            StrataPanicFn(b);
            LLVMBuildCall2(b->m_builder, b->m_panicFnType, b->m_panicFn, args2, 1, "");

            /* The `ret` here terminates the ENCLOSING Strata function, so it
               must use the enclosing function's return type (m_curRet), not the
               extern's. Returning the extern's type (e.g. a ptr for a `handle`
               return inside an i32 fn) produces malformed IR that crashes LLVM
               optimizers. */
            if (b->m_curRet.isVoid)
            {
                LLVMBuildRetVoid(b->m_builder);
            }
            else
            {
                LLVMBuildRet(b->m_builder, LLVMConstNull(b->m_curRetAbi));
            }
        }

        PositionAtEnd(b, callBB);
        call = LLVMBuildCall2(b->m_builder, info->type, callee, finalArgs, finalCount, "call");
    }
    else
    {
        call = LLVMBuildCall2(b->m_builder, info->type, callee, finalArgs, finalCount, "call");
    }

    /* Array-literal args: `ref T[]` temps are stack views (drop owning
       elements, never the buffer); by-value temps transferred ownership to
       the callee and are not touched here. */
    for (size_t t = 0; t < ownedArgTemps.count; t++)
    {
        OwnedArgTemp* at = (OwnedArgTemp*)VecGet(&ownedArgTemps, t);

        if (at->stackOnly)
        {
            EmitDropOneStack(b, at->slot, at->td);
        }
        else
        {
            EmitDropOne(b, at->slot, at->td);
        }
    }

    /* Sub-32-bit integer extern returns (bool/byte/sbyte/short/ushort): the
       C host only defines the low part of the widened register (AL/AX), so
       narrow the raw result back to the semantic type - truncating away the
       undefined upper bits, then re-extending per the semantic signedness.
       Strata-emitted callees always extend the full register themselves. */
    if (fd && fd->isExtern && info->retSemanticTy && call
        && LLVMGetTypeKind(info->retSemanticTy) == LLVMIntegerTypeKind)
    {
        LLVMTypeRef abiRet = LLVMGetReturnType(info->type);

        if (abiRet && LLVMGetTypeKind(abiRet) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(abiRet) > LLVMGetIntTypeWidth(info->retSemanticTy))
        {
            LLVMValueRef narrow = LLVMBuildTrunc(b->m_builder, call, info->retSemanticTy, "ret.narrow");

            bool zeroExtend = info->retSemanticUnsigned || info->retSemanticTy == I1Ty(b);

            call = zeroExtend ? LLVMBuildZExt(b->m_builder, narrow, abiRet, "ret.zext")
                              : LLVMBuildSExt(b->m_builder, narrow, abiRet, "ret.sext");
        }
    }

    /* Return-param call: the C function wrote the result into our temp slot;
       load it back as the Strata-level return value. */
    if (hasReturnParam)
    {
        LLVMValueRef retVal = LLVMBuildLoad2(b->m_builder, returnParamTd.type, returnParamSlot, "retparam.val");

        return ValueMake(retVal, returnParamTd);
    }

    /* Extern `string` return: the host handed back a NUL-terminated char*;
       copy it into a fresh owned buffer (the semantic type stays the fat).
       A DISCARDED result is skipped entirely — no allocation, no leak. */
    if (info->externStringReturn)
    {
        if (discarded)
        {
            return ValueMake(LLVMConstNull(ArrayStructType(b)), info->returnType);
        }

        Value owned = ValueMake(BuildOwnedStringFromCStr(b, call), info->returnType);
        NoteFreshOwned(b, owned);
        return owned;
    }

    Value result = ValueMake(call, info->returnType);

    /* A Strata function's owning result is a fresh value the caller owns
       (extern results keep their existing host-ownership handling). */
    if (fd && !fd->isExtern && !discarded)
    {
        NoteFreshOwned(b, result);
    }

    return result;
}
