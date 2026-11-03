/* Builder: Expressions: identifiers, lvalues, members, indexing, unary, conditionals. */
#include "Codegen/Builder/BuilderInternal.h"

/* The context the code being emitted runs against: the function's own, or (in code with none, such as a
   helper) the module's newest. NULL when the module has no instanced globals. */
LLVMValueRef CurrentContext(Builder* b)
{
    if (b->m_curGlobalsPtr)
    {
        return b->m_curGlobalsPtr;
    }

    return b->m_exportCtxGlobal ? LLVMBuildLoad2(b->m_builder, b->m_ptrTy, b->m_exportCtxGlobal, "curctx")
                                : LLVMConstNull(b->m_ptrTy);
}

/* Builds the getter/setter extern call for `m` (valueNode NULL = read).
   `found` reports whether `m` is an impl property at all. */
Value EmitImplProperty(Builder* b, MemberExpr* m, Node* valueNode, bool* found)
{
    *found = false;

    const char* handleName = StaticExprTypeName(b, m->base_node);

    if (!handleName || !TypeRegistryIsOpaque(&b->m_registry, handleName))
    {
        return ZeroInt(b);
    }

    ImplPropEntry* entry
        = (ImplPropEntry*)StrMapGet(&b->m_implProps, arena_format(b->m_arena, "%s.%s", handleName, m->member));

    if (!entry)
    {
        return ZeroInt(b);
    }

    *found = true;

    const FunctionDecl* acc = valueNode ? entry->setter : entry->getter;

    if (!acc)
    {
        return ZeroInt(b);
    }

    CallExpr tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.base.kind = NodeCall;
    tmp.base.range = m->base.range;
    tmp.callee = (char*)acc->mangledName;
    tmp.resolvedDecl = acc;
    VecInit(&tmp.args);
    VecPush(&tmp.args, m->base_node);

    if (valueNode)
    {
        VecPush(&tmp.args, valueNode);
    }

    Value result = EmitCall(b, &tmp);

    if (valueNode)
    {
        /* The setter returns void; the assignment-expression value is a
           well-typed zero of the property type so any use stays valid IR
           (sema rejects the chained-assignment form outright). */
        TypeDesc td = Resolve(b, &entry->decl->returnType);

        return ValueMake(ZeroOf(td), td);
    }

    return result;
}

LLVMValueRef ToI1(Builder* b, Value v)
{
    if (v.typeDesc.type == I1Ty(b))
    {
        return v.value;
    }

    /* An optional's truthiness is PRESENCE. A `T?` box is its (nullable)
       cell pointer; a `string?` / `T[]?` fat is empty as {null, 0}, so test
       the data pointer (a present value always has a non-null buffer). */
    if (TypeDescIsOptional(&v.typeDesc) && TypeDescHasArrayShape(&v.typeDesc))
    {
        LLVMValueRef data = LLVMBuildExtractValue(b->m_builder, v.value, 0, "opt.data");
        return LLVMBuildICmp(b->m_builder, LLVMIntNE, data, LLVMConstNull(b->m_ptrTy), "opt.has");
    }

    if (v.typeDesc.isFloat)
    {
        return LLVMBuildFCmp(b->m_builder, LLVMRealONE, v.value, LLVMConstNull(v.typeDesc.type), "tobool");
    }

    return LLVMBuildICmp(b->m_builder, LLVMIntNE, v.value, LLVMConstNull(v.typeDesc.type), "tobool");
}

static Value EmitIdent(Builder* b, IdentExpr* n)
{
    Value* sym = (Value*)StrMapGet(&b->m_symbols, n->name);
    bool isGlobal = false;

    if (!sym)
    {
        sym = (Value*)StrMapGet(&b->m_globals, n->name);
        isGlobal = sym != NULL;
    }

    if (!sym)
    {
        ConstValueSlot* cs = (ConstValueSlot*)StrMapGet(&b->m_constValues, n->name);

        if (cs)
        {
            /* Manifest constant: inline the value, no load. */
            return ValueMake(cs->constant, cs->td);
        }

        if (IsNullConstantName(n->name))
        {
            return ValueMake(LLVMConstNull(b->m_ptrTy), ResolveByName(b, STRATA_NULL_TYPE));
        }
    }

    if (!sym)
    {
        if (b->m_diag)
        {
            DiagErrorFmt(b->m_diag, n->base.range, "unknown identifier '%s'", n->name);
        }

        return ZeroInt(b);
    }

    LLVMValueRef addr = isGlobal ? GlobalFieldPtr(b, n->name) : sym->value;
    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, sym->typeDesc.type, addr, "id");

    return ValueMake(v, sym->typeDesc);
}

LValue EmitLValue(Builder* b, Node* n)
{
    LValue none = {0};

    if (!n)
    {
        return none;
    }

    if (n->kind == NodeIdent)
    {
        IdentExpr* id = (IdentExpr*)n;
        Value* sym = (Value*)StrMapGet(&b->m_symbols, id->name);
        bool isGlobal = false;

        if (!sym)
        {
            sym = (Value*)StrMapGet(&b->m_globals, id->name);
            isGlobal = sym != NULL;
        }

        if (!sym)
        {
            return none;
        }

        none.valid = true;
        none.ptr = isGlobal ? GlobalFieldPtr(b, id->name) : sym->value;
        none.typeDesc = sym->typeDesc;
        return none;
    }

    if (n->kind == NodeMember)
    {
        MemberExpr* m = (MemberExpr*)n;
        LValue base = EmitLValue(b, m->base_node);

        if (!base.valid)
        {
            return none;
        }

        /* array.length / array.cap — readable slots; typed uint (the fat's
           u32 fields). Assignment is rejected in sema. */
        if (TypeDescHasArrayShape(&base.typeDesc)
            && (strcmp(m->member, "length") == 0 || strcmp(m->member, "cap") == 0))
        {
            none.valid = true;
            none.ptr = strcmp(m->member, "length") == 0 ? ArrayLenPtr(b, base.ptr) : ArrayCapPtr(b, base.ptr);
            none.typeDesc = TypeDescMake(I32Ty(b), TD_UNSIGNED, NULL); /* uint */
            return none;
        }

        /* Fixed-array .length is a compile-time constant, not an lvalue. */
        if (base.typeDesc.isFixedArray && strcmp(m->member, "length") == 0)
        {
            return none;
        }

        LLVMValueRef structPtr;
        LLVMTypeRef structTy;
        const char* structName;

        if (TypeDescIsBox(&base.typeDesc))
        {
            if (!base.typeDesc.boxInner)
            {
                return none;
            }

            /* A boxed SIMD vector: the cell holds the vector value itself, so
               a lane lvalue points at the cell (load vector, insert, store
               back through it). */
            if (IsNameSimdVector(base.typeDesc.boxInner->name))
            {
                return MakeVectorLaneLValue(b, none, m, LLVMBuildLoad2(b->m_builder, b->m_ptrTy, base.ptr, "box"),
                                            Resolve(b, base.typeDesc.boxInner));
            }

            structPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, base.ptr, "box");
            structName = base.typeDesc.boxInner->name;
            structTy = Resolve(b, base.typeDesc.boxInner).type;
        }
        else if (base.typeDesc.structTypeName)
        {
            structPtr = base.ptr;
            structName = base.typeDesc.structTypeName;
            structTy = base.typeDesc.type;
        }
        else if (base.typeDesc.isSimdVector)
        {
            return MakeVectorLaneLValue(b, none, m, base.ptr, base.typeDesc);
        }
        else
        {
            return none;
        }

        int idx = TypeRegistryFieldIndex(&b->m_registry, structName, m->member);

        if (idx < 0)
        {
            return none;
        }

        const StructType* st = TypeRegistryFind(&b->m_registry, structName);

        FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, (size_t)idx);
        TypeDesc fieldTypeDesc = Resolve(b, &fieldDecl->type);

        unsigned physIdx = PhysicalFieldIndex(st, idx);

        LLVMValueRef idxs[2] = {0};
        idxs[0] = IdxConst(b, 0);
        idxs[1] = IdxConst(b, physIdx);

        LLVMValueRef ptr = LLVMBuildGEP2(b->m_builder, structTy, structPtr, idxs, 2, "f");

        none.valid = true;
        none.ptr = ptr;
        none.typeDesc = fieldTypeDesc;

        return none;
    }

    if (n->kind == NodeIndex)
    {
        IndexExpr* ix = (IndexExpr*)n;
        LValue base = EmitLValue(b, ix->base_node);

        if (base.valid && base.typeDesc.isCString)
        {
            /* cstring[i]: load the pointer, bounds-check against strlen, and
               address the byte. Sema rejects writes, so this is a read view. */
            LLVMValueRef ptr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, base.ptr, "cs");
            LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, ix->index));
            LLVMValueRef lenVal = BuildStrLen(b, ptr);
            TypeDesc elemTd = TypeDescMake(LLVMInt8TypeInContext(b->m_ctx), TD_UNSIGNED, NULL);

            none.valid = true;
            none.ptr = EmitCheckedElemPtr(b, idxVal, lenVal, ptr, elemTd.type, elemTd, false);
            none.typeDesc = elemTd;

            return none;
        }

        if (!base.valid || !TypeDescHasArrayShape(&base.typeDesc))
        {
            if (!base.valid || !base.typeDesc.isFixedArray)
            {
                return none;
            }

            /* Fixed inline array: GEP into the [N x T]; length is the compile-time dimension. */
            TypeDesc elemTd = Resolve(b, base.typeDesc.arrayInner);
            LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

            LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, ix->index));

            none.valid = true;
            none.ptr
                = EmitCheckedFixedElemPtr(b, idxVal, base.typeDesc.fixedLength, base.ptr, base.typeDesc.type, elemTd);
            none.typeDesc = elemTd;

            return none;
        }

        LLVMValueRef dataPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, base.ptr), "ap");

        /* string[i]: the element is a byte (arrayInner is unused for strings). */
        TypeDesc elemTd = base.typeDesc.isString ? TypeDescMake(LLVMInt8TypeInContext(b->m_ctx), TD_UNSIGNED, NULL)
                                                 : Resolve(b, base.typeDesc.arrayInner);
        LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

        LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, ix->index));
        LLVMValueRef lenVal = WidenLen(b, LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, base.ptr), "len"));

        none.valid = true;
        none.ptr = EmitCheckedElemPtr(b, idxVal, lenVal, dataPtr, elemTy, elemTd, base.typeDesc.aliasedArray);
        none.typeDesc = elemTd;

        return none;
    }

    return none;
}

/* Coerces an integer/float index value to an i64 GEP index. */
LLVMValueRef AsI64Index(Builder* b, Value v)
{
    if (v.typeDesc.isFloat)
    {
        return LLVMBuildFPToSI(b->m_builder, v.value, I64Ty(b), "i");
    }

    if (v.typeDesc.type != I64Ty(b))
    {
        return LLVMBuildIntCast2(b->m_builder, v.value, I64Ty(b), !v.typeDesc.isUnsigned, "i");
    }

    return v.value;
}

static Value EmitMember(Builder* b, MemberExpr* n)
{
    /* Scoped enum constant: `EnumName.Member` — emit the underlying constant
       value inline. */
    if (n->isEnumConst)
    {
        TypeName tn = TypeNameLeaf((char*)n->enumTypeName);
        TypeDesc td = Resolve(b, &tn);
        LLVMValueRef v = LLVMConstInt(td.type, n->enumValue, td.isUnsigned);
        return ValueMake(v, td);
    }

    /* Builtin scalar pseudo-property: `int.max` / `float.min` — a compile
       time constant of the base scalar's own type. */
    if (n->isScalarConst)
    {
        TypeName tn = TypeNameLeaf((char*)((IdentExpr*)n->base_node)->name);
        TypeDesc td = Resolve(b, &tn);

        uint64_t intVal = 0;
        double floatVal = 0.0;
        bool isFloat = false;

        if (ScalarPseudoConst(tn.primitiveType, n->member, &intVal, &floatVal, &isFloat))
        {
            LLVMValueRef v = isFloat ? LLVMConstReal(td.type, floatVal) : LLVMConstInt(td.type, intVal, td.isUnsigned);
            return ValueMake(v, td);
        }
    }

    /* Impl property read: lower to the getter extern call. Checked first —
       handles have no fields, so the lvalue path could never serve them. */
    bool isProp = false;
    Value prop = EmitImplProperty(b, n, NULL, &isProp);

    if (isProp)
    {
        return prop;
    }

    LValue lvalue = EmitLValue(b, (Node*)n);

    if (lvalue.valid)
    {
        if (lvalue.vectorLane)
        {
            /* Single-lane vector read: load the vector, extract the lane. */
            LLVMValueRef vec = LLVMBuildLoad2(b->m_builder, lvalue.vectorType, lvalue.ptr, "mvec");
            LLVMValueRef v = LLVMBuildExtractElement(b->m_builder, vec, LLVMConstInt(I32Ty(b), lvalue.lane, 0), "m");
            return ValueMake(v, lvalue.typeDesc);
        }

        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, lvalue.typeDesc.type, lvalue.ptr, "m");

        return ValueMake(v, lvalue.typeDesc);
    }

    Value base = EmitExpr(b, n->base_node);

    /* array.data / string.data: the element buffer as a raw `ptr` (NULL when empty). */
    if ((TypeDescHasArrayShape(&base.typeDesc) || base.typeDesc.isString) && strcmp(n->member, "data") == 0)
    {
        TypeDesc ptrTd = ResolveByName(b, STRATA_PTR_TYPE);
        LLVMValueRef v = LLVMBuildExtractValue(b->m_builder, base.value, 0, "data");
        BorrowTemp(b, base, ptrTd);
        return ValueMake(v, ptrTd);
    }

    /* array.length / array.cap on a non-lvalue array (e.g. MakeArr().length). */
    if (TypeDescHasArrayShape(&base.typeDesc) && (strcmp(n->member, "length") == 0 || strcmp(n->member, "cap") == 0))
    {
        unsigned field = strcmp(n->member, "length") == 0 ? 1 : 2;
        LLVMValueRef v = LLVMBuildExtractValue(b->m_builder, base.value, field, n->member);
        TypeDesc lenTd = TypeDescMake(I32Ty(b), TD_UNSIGNED, NULL);
        BorrowTemp(b, base, lenTd);
        return ValueMake(v, lenTd);
    }

    /* Fixed-array .length: the compile-time dimension as a constant. */
    if (base.typeDesc.isFixedArray && strcmp(n->member, "length") == 0)
    {
        LLVMValueRef len = LLVMConstInt(I32Ty(b), (unsigned long long)base.typeDesc.fixedLength, 0);
        return ValueMake(len, TypeDescMake(I32Ty(b), TD_UNSIGNED, NULL));
    }

    /* cstring .length: runtime strlen (NUL-terminated), typed uint. */
    if (base.typeDesc.isCString && strcmp(n->member, "length") == 0)
    {
        LLVMValueRef len = BuildStrLen(b, base.value);
        return ValueMake(LLVMBuildTrunc(b->m_builder, len, I32Ty(b), "cslen"),
                         TypeDescMake(I32Ty(b), TD_UNSIGNED, NULL));
    }

    if (TypeDescIsBox(&base.typeDesc) && base.typeDesc.boxInner)
    {
        /* A box-typed value with no addressable lvalue (e.g. a call result
            used directly: `MakeThing().field`) - GEP+load through the
            pointer instead of extracting from an aggregate value. */
        LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, base.typeDesc.boxInner->name);
        int idx = TypeRegistryFieldIndex(&b->m_registry, base.typeDesc.boxInner->name, n->member);

        if (structTy && idx >= 0)
        {
            const StructType* st = TypeRegistryFind(&b->m_registry, base.typeDesc.boxInner->name);
            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, (size_t)idx);
            TypeDesc fieldTypeDesc = Resolve(b, &fieldDecl->type);

            LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, idx))};
            LLVMValueRef ptr = LLVMBuildGEP2(b->m_builder, structTy, base.value, idxs, 2, "f");
            LLVMValueRef v = LLVMBuildLoad2(b->m_builder, fieldTypeDesc.type, ptr, "m");

            if (TypeDescIsOwning(b, &fieldTypeDesc))
            {
                BorrowTempPart(b, base, n->base_node, (Node*)n, fieldTypeDesc, ptr, NULL, 0);
            }
            else
            {
                BorrowTemp(b, base, fieldTypeDesc);
            }

            return ValueMake(v, fieldTypeDesc);
        }
    }

    if (TypeDescIsBox(&base.typeDesc) && base.typeDesc.boxInner && IsNameSimdVector(base.typeDesc.boxInner->name))
    {
        /* Resolve a box (surrounding a vector) down to the vector type. */
        TypeDesc innerType = Resolve(b, base.typeDesc.boxInner);
        base.value = LLVMBuildLoad2(b->m_builder, innerType.type, base.value, "boxvec");
        base.typeDesc = innerType;
    }

    if (base.typeDesc.isSimdVector)
    {
        LLVMTypeRef floatType = LLVMFloatTypeInContext(b->m_ctx);

        LLVMValueRef dsValue = LSimdVectorDestructure(b, base.value, n);

        if (dsValue != NULL)
        {
            TypeDesc dsTd = TypeDescMake(floatType, TD_FLOAT | TD_VECTOR, NULL);
            size_t comps = strlen(n->member);
            dsTd.simdLanes = comps > 1 ? (int)comps : 0;
            return (Value){dsValue, dsTd};
        }
    }

    if (base.typeDesc.structTypeName)
    {
        int idx = TypeRegistryFieldIndex(&b->m_registry, base.typeDesc.structTypeName, n->member);

        if (idx >= 0)
        {
            const StructType* st = TypeRegistryFind(&b->m_registry, base.typeDesc.structTypeName);

            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, (size_t)idx);
            TypeDesc fieldTypeDesc = Resolve(b, &fieldDecl->type);

            LLVMValueRef v = ExtractStructField(b, st, base.value, idx, fieldTypeDesc.type);

            if (TypeDescIsOwning(b, &fieldTypeDesc))
            {
                BorrowTempPart(b, base, n->base_node, (Node*)n, fieldTypeDesc, NULL, base.typeDesc.type,
                               (int)PhysicalFieldIndex(st, idx));
            }
            else
            {
                BorrowTemp(b, base, fieldTypeDesc);
            }

            return ValueMake(v, fieldTypeDesc);
        }
    }

    if (b->m_diag)
    {
        DiagErrorFmt(b->m_diag, n->base.range, "cannot access member '%s'", n->member);
    }

    return ZeroInt(b);
}

static Value EmitIndex(Builder* b, IndexExpr* n)
{
    /* Prefer the addressable path: arr[i], obj.field[i], etc. */
    LValue lv = EmitLValue(b, (Node*)n);

    if (lv.valid)
    {
        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, lv.typeDesc.type, lv.ptr, "el");

        return ValueMake(v, lv.typeDesc);
    }

    /* Fallback: base is a non-addressable array rvalue (literal or call
       result), e.g. {1,2,3}[0] or MakeArr()[2]. */
    Value base = EmitExpr(b, n->base_node);

    if (base.typeDesc.isFixedArray)
    {
        /* Fixed inline array rvalue: park in an entry alloca so a runtime index can address it. */
        TypeDesc elemTd = Resolve(b, base.typeDesc.arrayInner);

        LLVMValueRef slot = EntryAlloca(b, base.typeDesc.type, "fxr");
        LLVMBuildStore(b->m_builder, base.value, slot);

        LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, n->index));

        LLVMValueRef elemAddr
            = EmitCheckedFixedElemPtr(b, idxVal, base.typeDesc.fixedLength, slot, base.typeDesc.type, elemTd);
        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, elemTd.type, elemAddr, "el");

        return ValueMake(v, elemTd);
    }

    if (!TypeDescHasArrayShape(&base.typeDesc))
    {
        if (b->m_diag)
        {
            DiagErrorFmt(b->m_diag, n->base.range, "indexing a non-array value");
        }

        return ZeroInt(b);
    }

    LLVMValueRef dataPtr = LLVMBuildExtractValue(b->m_builder, base.value, 0, "ap");

    /* string[i]: the element is a byte (arrayInner is unused for strings). */
    TypeDesc elemTd = base.typeDesc.isString ? TypeDescMake(LLVMInt8TypeInContext(b->m_ctx), TD_UNSIGNED, NULL)
                                             : Resolve(b, base.typeDesc.arrayInner);
    LLVMTypeRef elemTy = TypeDescHasArrayShape(&elemTd) ? ArrayStructType(b) : elemTd.type;

    LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, n->index));
    LLVMValueRef lenVal = WidenLen(b, LLVMBuildExtractValue(b->m_builder, base.value, 1, "len"));

    LLVMValueRef elemAddr = EmitCheckedElemPtr(b, idxVal, lenVal, dataPtr, elemTy, elemTd, false);
    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, elemTy, elemAddr, "el");

    if (TypeDescIsOwning(b, &elemTd))
    {
        BorrowTempPart(b, base, n->base_node, (Node*)n, elemTd, elemAddr, NULL, 0);
    }
    else
    {
        BorrowTemp(b, base, elemTd);
    }

    return ValueMake(v, elemTd);
}

/* Evaluates a boolean-context operand (`&&`, `||`, `!`). An optional stays
   un-dereferenced so ToI1 tests presence, exactly like `if (o)`; loading an
   empty optional's cell would dereference null. Other boxes read through. */
Value EmitCondOperand(Builder* b, Node* n)
{
    Value v = EmitExpr(b, n);
    return TypeDescIsOptional(&v.typeDesc) ? v : DerefBoxValue(b, v);
}

static Value EmitUnary(Builder* b, UnaryExpr* n)
{
    Value e = n->op == UnNot ? EmitCondOperand(b, n->operand) : DerefBoxValue(b, EmitExpr(b, n->operand));

    switch (n->op)
    {
    case UnPos:
        return e;

    case UnNeg:
    {
        LLVMValueRef ref = e.typeDesc.isFloat ? LLVMBuildFNeg(b->m_builder, e.value, "neg")
                                              : LLVMBuildNeg(b->m_builder, e.value, "neg");

        return ValueMake(ref, e.typeDesc);
    }

    case UnNot:
    {
        LLVMValueRef v = ToI1(b, e);
        LLVMValueRef ref = LLVMBuildXor(b->m_builder, v, LLVMConstInt(I1Ty(b), 1, 0), "not");

        return ValueMake(ref, TypeDescMake(I1Ty(b), 0, NULL));
    }

    case UnBitNot:
    {
        LLVMValueRef ref = LLVMBuildNot(b->m_builder, e.value, "bnot");

        return ValueMake(ref, e.typeDesc);
    }
    }

    return e;
}

/* The value a slot holds, copied aside. Overwriting stores the NEW value first and then drops this copy, so a
   `Drop` that writes the same slot meets a slot that already holds the new value instead of the half-destroyed
   old one (and the value it stores is not overwritten and leaked). */
LLVMValueRef SaveOldValue(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    LLVMTypeRef ty = TypeDescHasArrayShape(&td) ? ArrayStructType(b) : td.type;
    LLVMValueRef saved = EntryAlloca(b, ty, "oldval");
    LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, ty, slot, "old"), saved);

    return saved;
}

/* One branch of `?:`, converted to the result type; its temporaries die inside the branch. */
static LLVMValueRef EmitConditionalBranch(Builder* b, Node* n, TypeDesc td)
{
    size_t tempMark = b->m_temps.count;
    LLVMValueRef v;

    if (td.isCString && n->kind == NodeStrLiteral)
    {
        v = EmitCStringLiteralPtr(b, (const StrLiteral*)n);
    }
    else
    {
        v = Coerce(b, EmitExpr(b, n), td).value;
    }

    FlushTemps(b, tempMark);
    return v;
}

static Value EmitConditional(Builder* b, ConditionalExpr* ce)
{
    TypeDesc td = Resolve(b, ce->resultType);
    LLVMValueRef cond = EmitConditionI1(b, ce->condition);

    LLVMBasicBlockRef thenBB = NewBb(b, "cond.then");
    LLVMBasicBlockRef elseBB = NewBb(b, "cond.else");
    LLVMBasicBlockRef endBB = NewBb(b, "cond.end");
    LLVMBuildCondBr(b->m_builder, cond, thenBB, elseBB);

    PositionAtEnd(b, thenBB);
    LLVMValueRef thenValue = EmitConditionalBranch(b, ce->thenExpr, td);
    LLVMBasicBlockRef thenEnd = LLVMGetInsertBlock(b->m_builder);
    LLVMBuildBr(b->m_builder, endBB);

    PositionAtEnd(b, elseBB);
    LLVMValueRef elseValue = EmitConditionalBranch(b, ce->elseExpr, td);
    LLVMBasicBlockRef elseEnd = LLVMGetInsertBlock(b->m_builder);
    LLVMBuildBr(b->m_builder, endBB);

    PositionAtEnd(b, endBB);
    LLVMValueRef phi = LLVMBuildPhi(b->m_builder, td.type, "cond");
    LLVMValueRef values[2] = {thenValue, elseValue};
    LLVMBasicBlockRef blocks[2] = {thenEnd, elseEnd};
    LLVMAddIncoming(phi, values, blocks, 2);

    return ValueMake(phi, td);
}

Value EmitExpr(Builder* b, Node* n)
{
    if (!n)
    {
        return ZeroInt(b);
    }

    switch (n->kind)
    {
    case NodeIntLiteral:
    {
        IntLiteral* literal = (IntLiteral*)n;

        /* A magnitude that can't be represented as a non-negative signed
           i32 (>= 2^31) must be a 64-bit literal - otherwise LLVMConstInt on
           an i32 sign-wraps it and the value is lost (e.g. `long a = 3000000000`
           previously became a negative i32). */
        if (literal->value > 0x7FFFFFFFULL || literal->isLong)
        {
            TypeDesc typeDesc = TypeDescMake(I64Ty(b), (literal->isUnsigned ? TD_UNSIGNED : 0), NULL);

            return ValueMake(LLVMConstInt(I64Ty(b), literal->value, 1), typeDesc);
        }

        TypeDesc typeDesc = TypeDescMake(I32Ty(b), (literal->isUnsigned ? TD_UNSIGNED : 0), NULL);

        return ValueMake(LLVMConstInt(I32Ty(b), literal->value, 1), typeDesc);
    }

    case NodeFloatLiteral:
    {
        FloatLiteral* literal = (FloatLiteral*)n;

        LLVMTypeRef fty = LLVMFloatTypeInContext(b->m_ctx);
        TypeDesc typeDesc = TypeDescMake(fty, TD_FLOAT, NULL);

        return ValueMake(LLVMConstReal(fty, literal->value), typeDesc);
    }

    case NodeBoolLiteral:
    {
        BoolLiteral* literal = (BoolLiteral*)n;

        TypeDesc typeDesc = TypeDescMake(I1Ty(b), 0, NULL);

        return ValueMake(LLVMConstInt(I1Ty(b), (unsigned long long)literal->value, 0), typeDesc);
    }

    case NodeStrLiteral:
    {
        StrLiteral* literal = (StrLiteral*)n;

        size_t len = literal->length;
        /* The constant carries its NUL terminator (flag 0 in this LLVM
           build), upholding the fat-string invariant: NUL at [len]. */
        LLVMValueRef strConst = LLVMConstStringInContext(b->m_ctx, literal->value, (unsigned)len, 0);
        LLVMTypeRef strType = LLVMTypeOf(strConst);

        char* gName = arena_format(b->m_arena, ".str.%d", b->m_strLitCount++);
        LLVMValueRef global = LLVMAddGlobal(b->m_mod, strType, gName);
        LLVMSetInitializer(global, strConst);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(global, 1);
        LLVMSetGlobalConstant(global, 1);

        LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
        LLVMValueRef idx[2] = {zero, zero};
        LLVMValueRef gep = LLVMConstGEP2(strType, global, idx, 2);

        /* A literal is a borrowed fat {ptr, len, cap}: no allocation, NUL at
           [len]. cap is len+1 (the constant carries the terminator byte).
           Owning consumers heap-copy via EmitOwnedValue. */
        TypeDesc td = StringFatDesc(b);
        LLVMValueRef fat
            = LLVMConstNamedStruct(ArrayStructType(b),
                                   (LLVMValueRef[3]){gep, LLVMConstInt(I32Ty(b), (unsigned long long)len, 0),
                                                     LLVMConstInt(I32Ty(b), (unsigned long long)len + 1, 0)},
                                   3);

        return ValueMake(fat, td);
    }

    case NodeIdent:
        return EmitIdent(b, (IdentExpr*)n);

    case NodeUnary:
        return EmitUnary(b, (UnaryExpr*)n);

    case NodeBinary:
        return EmitBinary(b, (BinaryExpr*)n);

    case NodeAssign:
        return EmitAssign(b, (AssignExpr*)n);

    case NodeCall:
        return EmitCall(b, (CallExpr*)n);

    case NodeMember:
        return EmitMember(b, (MemberExpr*)n);

    case NodeIndex:
        return EmitIndex(b, (IndexExpr*)n);

    case NodeArrayInit:
        return EmitArrayInit(b, (ArrayInitExpr*)n);

    case NodeStructInit:
        return EmitStructInit(b, (StructInitExpr*)n);

    case NodeConditional:
        return EmitConditional(b, (ConditionalExpr*)n);

    case NodeNullTest:
    {
        /* `expr?` - test whether the optional holds anything (zero cost
           beyond the compare; sema guarantees a nullable path). A T?/^T slot
           tests the pointer; an optional array tests its fat struct: set =
           non-null data OR non-zero length. */
        NullTestExpr* nt = (NullTestExpr*)n;
        LValue lv = EmitLValue(b, nt->operand);

        if (lv.valid && TypeDescHasArrayShape(&lv.typeDesc))
        {
            LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "oad");
            LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I32Ty(b), ArrayLenPtr(b, lv.ptr), "oal");

            LLVMValueRef dataNonNull = LLVMBuildICmp(b->m_builder, LLVMIntNE, data, LLVMConstNull(b->m_ptrTy), "nn");
            LLVMValueRef lenNonZero = LLVMBuildICmp(b->m_builder, LLVMIntNE, len, LLVMConstInt(I32Ty(b), 0, 0), "nz");

            return ValueMake(LLVMBuildOr(b->m_builder, dataNonNull, lenNonZero, "opt"), TypeDescMake(I1Ty(b), 0, NULL));
        }

        /* Testing a temporary (`Make()?`) only reads it: the end of the statement drops it. */
        Value tested = {0};

        if (!lv.valid)
        {
            tested = EmitExpr(b, nt->operand);
            ClaimTemp(b, tested);
        }

        LLVMValueRef slotPtr = lv.valid ? lv.ptr : tested.value;
        LLVMValueRef boxPtr = lv.valid ? LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slotPtr, "opt") : slotPtr;

        LLVMValueRef isNonNull = LLVMBuildICmp(b->m_builder, LLVMIntNE, boxPtr, LLVMConstNull(b->m_ptrTy), "nn");

        return ValueMake(isNonNull, TypeDescMake(I1Ty(b), 0, NULL));
    }

    case NodeIncDec:
    {
        IncDecExpr* inc = (IncDecExpr*)n;
        LValue lv = EmitLValue(b, inc->operand);

        if (!lv.valid)
        {
            if (b->m_diag)
            {
                DiagError(b->m_diag, inc->base.range, "increment/decrement of non-lvalue");
            }

            return ValueMake(NULL, TypeDescMake(NULL, TD_VOID, NULL));
        }

        if (lv.vectorLane)
        {
            /* SIMD lane ++/--: load the vector, adjust the lane, store back. */
            LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, lv.vectorType, lv.ptr, "lanev");
            LLVMValueRef laneVal
                = LLVMBuildExtractElement(b->m_builder, cur, LLVMConstInt(I32Ty(b), lv.lane, 0), "lanecur");
            LLVMValueRef one = LLVMConstReal(LLVMFloatTypeInContext(b->m_ctx), 1.0);
            LLVMValueRef newVal = inc->isDec ? LLVMBuildFSub(b->m_builder, laneVal, one, "dec")
                                             : LLVMBuildFAdd(b->m_builder, laneVal, one, "inc");
            LLVMValueRef newVec
                = LLVMBuildInsertElement(b->m_builder, cur, newVal, LLVMConstInt(I32Ty(b), lv.lane, 0), "laneins");
            LLVMBuildStore(b->m_builder, newVec, lv.ptr);

            return ValueMake(inc->isPrefix ? newVal : laneVal, lv.typeDesc);
        }

        LLVMValueRef valPtr = lv.ptr;
        TypeDesc valTd = lv.typeDesc;

        if (TypeDescIsBox(&lv.typeDesc))
        {
            /* lv.ptr is the address of the box pointer slot (ref-adjusted
               already by EmitLValue); load through it to get the box
               pointer, then read/write the boxed value through that -
               same pattern as the box branch in EmitAssign. */
            valPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "box");
            valTd = Resolve(b, lv.typeDesc.boxInner);
        }

        LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, valTd.type, valPtr, "inc");
        LLVMValueRef one = valTd.isFloat ? LLVMConstReal(valTd.type, 1.0) : LLVMConstInt(valTd.type, 1, 0);

        LLVMValueRef newVal = inc->isDec ? (valTd.isFloat ? LLVMBuildFSub(b->m_builder, cur, one, "dec")
                                                          : LLVMBuildSub(b->m_builder, cur, one, "dec"))
                                         : (valTd.isFloat ? LLVMBuildFAdd(b->m_builder, cur, one, "inc")
                                                          : LLVMBuildAdd(b->m_builder, cur, one, "inc"));

        LLVMBuildStore(b->m_builder, newVal, valPtr);

        return ValueMake(inc->isPrefix ? newVal : cur, valTd);
    }

    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;

        if (cast->textKind != CastTextNone)
        {
            return EmitTextCast(b, cast);
        }

        Value operand = EmitExpr(b, cast->operand);
        TypeDesc target = Resolve(b, &cast->type);

        EmitEnumCastCheck(b, cast, operand, target);

        /* `(Name)"lit"` / `(string)"lit"`: the result must OWN its bytes, so
           heap-copy the static literal (drop glue frees real memory, never
           the constant pool). Movable sources stay a retag - the consumer
           nulls the source, exactly like `string t = s;`. */
        if (target.isString && cast->operand->kind == NodeStrLiteral)
        {
            LLVMValueRef srcPtr = LLVMBuildExtractValue(b->m_builder, operand.value, 0, "clit.p");
            LLVMValueRef lenVal = LLVMBuildExtractValue(b->m_builder, operand.value, 1, "clit.l");
            LLVMValueRef owned = BuildOwnedStringFat(b, srcPtr, lenVal);

            return ValueMake(owned, target);
        }

        if ((TypeDescIsBox(&operand.typeDesc) && TypeDescIsBox(&target))
            || (operand.typeDesc.isString && target.isString))
        {
            /* Retag with the destination type; Coerce would keep the source's. */
            return ValueMake(operand.value, target);
        }

        /* Pointer to pointer (handle <-> handle, ptr <-> cstring): same bits, the destination's type. */
        bool operandIsPointer = operand.typeDesc.type && LLVMGetTypeKind(operand.typeDesc.type) == LLVMPointerTypeKind
                                && !TypeDescIsBox(&operand.typeDesc);
        bool targetIsPointer
            = target.type && LLVMGetTypeKind(target.type) == LLVMPointerTypeKind && !TypeDescIsBox(&target);

        if (operandIsPointer && targetIsPointer)
        {
            return ValueMake(operand.value, target);
        }

        return Coerce(b, operand, target);
    }

    default:
        if (b->m_diag)
        {
            DiagError(b->m_diag, n->range, "unsupported expression");
        }

        return ZeroInt(b);
    }
}

/* Emits an expression whose result is discarded (expression statement, `for`
   update). A direct call's OWNING result is spilled and dropped so the
   temporary doesn't leak; an extern `string` return is flagged (m_discardCallResult)
   so its char* copy is skipped entirely. */
void EmitDiscardExpr(Builder* b, Node* expr)
{
    if (!expr)
    {
        return;
    }

    bool directCall = expr->kind == NodeCall;
    b->m_discardCallResult = directCall;
    Value v = EmitExpr(b, expr);
    b->m_discardCallResult = false;

    if (directCall && TypeDescIsOwning(b, &v.typeDesc))
    {
        LLVMValueRef slot = EntryAlloca(b, v.typeDesc.type, "discard");
        LLVMBuildStore(b->m_builder, v.value, slot);
        EmitDropOne(b, slot, v.typeDesc);
    }
}

/* A branch/loop condition as i1, with its borrowed temporaries dropped
   before the branch (the i1 no longer refers to them). `^bool` reads
   through like `&&`/`!` operands; an optional tests presence. */
LLVMValueRef EmitConditionI1(Builder* b, Node* cond)
{
    size_t tempMark = b->m_temps.count;
    LLVMValueRef v = ToI1(b, EmitCondOperand(b, cond));

    FlushTemps(b, tempMark);
    return v;
}
