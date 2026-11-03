/* Builder: Type descriptors, resolution, coercion, default values, owning-type queries. */
#include "Codegen/Builder/BuilderInternal.h"

/* Forward declarations for functions used before their definition in this file. */
static LLVMValueRef TypeDefaultConstant(Builder* b, const TypeName* type);

TypeDesc TypeDescMake(LLVMTypeRef type, TypeDescFlags flags, const char* structTypeName)
{
    TypeDesc td = {0};
    td.type = type;
    td.flags = flags;
    td.structTypeName = structTypeName;

    return td;
}

Value ValueMake(LLVMValueRef value, TypeDesc typeDesc)
{
    Value v = {0};
    v.value = value;
    v.typeDesc = typeDesc;

    return v;
}

TypeName MakeTypeName(Builder* b, const char* name)
{
    return TypeNameParse(b->m_arena, name);
}

/* Resolve from a canonical spelling — only for the few spots that genuinely
   start from a plain name (e.g. "string"). */
TypeDesc ResolveByName(Builder* b, const char* name)
{
    TypeName tn = MakeTypeName(b, name);
    return Resolve(b, &tn);
}

const TypeName* StringTypeName(Builder* b)
{
    TypeName* t = (TypeName*)arena_alloc(b->m_arena, sizeof(TypeName));
    *t = TypeNameLeaf((char*)"string");
    return t;
}

static LLVMTypeRef ScalarLlvmType(LLVMContextRef ctx, const MappedType* t)
{
    assert(t->lanes <= 1);

    if (t->isVoid)
    {
        return LLVMVoidTypeInContext(ctx);
    }

    if (strcmp(t->elemIr, "i1") == 0)
    {
        return LLVMInt1TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i8") == 0)
    {
        return LLVMInt8TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i16") == 0)
    {
        return LLVMInt16TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i32") == 0)
    {
        return LLVMInt32TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i64") == 0)
    {
        return LLVMInt64TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "float") == 0)
    {
        return LLVMFloatTypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "double") == 0)
    {
        return LLVMDoubleTypeInContext(ctx);
    }

    return LLVMInt32TypeInContext(ctx);
}

/* SIMD vectors route here (lanes > 1); ScalarLlvmType stays scalar-only. */
static LLVMTypeRef VectorLlvmType(LLVMContextRef ctx, const MappedType* t)
{
    assert(t->isSimdVector);
    assert(t->lanes > 1);

    LLVMTypeRef elem = strcmp(t->elemIr, "double") == 0 ? LLVMDoubleTypeInContext(ctx) : LLVMFloatTypeInContext(ctx);

    return LLVMVectorType(elem, (unsigned int)t->lanes);
}

// Owning when the type itself owns, or its alias resolves to one.
bool BuilderIsOwningType(Builder* b, const TypeName* t)
{
    return TypeIsOwningResolved(&b->m_registry, b->m_arena, t);
}

// Owning VALUE: BuilderIsOwningType plus owning structs (and aliases) — a
// `Rec` value owns its fields. Drives drop/copy/move glue wherever an
// owning-struct value can appear, notably as an inline `Rec[]` element.
bool BuilderIsOwningValue(Builder* b, const TypeName* t)
{
    return TypeIsOwningValueResolved(&b->m_registry, b->m_arena, t);
}

/* A TypeDesc that owns heap memory and must be dropped: string / box (a heap
   pointer), a dynamic array (its buffer, plus owning elements), or an owning
   struct held by value. Fixed arrays / scalars are non-owning. */
bool TypeDescIsOwning(Builder* b, const TypeDesc* td)
{
    if (TypeDescIsString(td) || TypeDescIsBox(td) || TypeDescHasArrayShape(td))
    {
        return true;
    }

    return td->structTypeName && TypeRegistryIsOwningStruct(&b->m_registry, td->structTypeName);
}

TypeDesc Resolve(Builder* b, const TypeName* t)
{
    if (!t)
    {
        return TypeDescMake(NULL, TDF_VOID, NULL);
    }

    MappedType mapped = MapType(t);

    if (mapped.valid)
    {
        TypeDescFlags flags = 0;
        if (mapped.isFloat)
        {
            flags |= TDF_FLOAT;
        }
        if (mapped.isUnsigned)
        {
            flags |= TDF_UNSIGNED;
        }
        if (mapped.isVoid)
        {
            flags |= TDF_VOID;
        }
        if (mapped.isSimdVector)
        {
            flags |= TDF_SIMD_VECTOR;

            TypeDesc td = TypeDescMake(VectorLlvmType(b->m_ctx, &mapped), flags, NULL);
            td.simdLanes = IsNameSimdVector(t->name);
            return td;
        }

        return TypeDescMake(ScalarLlvmType(b->m_ctx, &mapped), flags, NULL);
    }

    /* Type alias: resolve to the underlying type's LLVM representation. */
    if (TypeRegistryIsTypeAlias(&b->m_registry, t->name))
    {
        const char* underlying = TypeRegistryGetUnderlyingType(&b->m_registry, t->name);
        TypeName resolved = MakeTypeName(b, underlying);

        /* Propagate const/vector/array flags from the alias wrapper. */
        resolved.isConst = t->isConst;
        resolved.isVector = t->isVector;

        return Resolve(b, &resolved);
    }

    LLVMTypeRef found = (LLVMTypeRef)StrMapGet(&b->m_structTypes, t->name);

    if (found)
    {
        return TypeDescMake(found, 0, t->name);
    }

    /* T[N]: fixed-size inline [N x T] (C ABI) — struct fields and stack-allocated locals. */
    if (TypeNameHasArrayShape(t) && t->length >= 0)
    {
        TypeDesc elemTd = Resolve(b, t->elem);

        TypeDesc td = {0};
        td.type = LLVMArrayType(elemTd.type, (unsigned)t->length);
        TypeDescSetFixedArray(&td, true);
        td.fixedLength = t->length;
        td.arrayInner = t->elem;

        return td;
    }

    if (TypeNameHasArrayShape(t))
    {
        // T[] is a fat {ptr, len, cap} struct.
        TypeDesc td = {0};
        td.type = ArrayStructType(b);
        TypeDescSetArrayShape(&td, true);
        td.arrayInner = t->elem;

        return td;
    }

    // T[]? shares the T[] representation; the flag is sema-only.
    if (TypeNameIsOptional(t) && t->inner && TypeNameHasArrayShape(t->inner) && t->inner->length < 0)
    {
        TypeDesc td = Resolve(b, t->inner);
        TypeDescSetOptional(&td, true);
        return td;
    }

    // Optional string: same fat as string, empty is {null, 0}.
    if (TypeNameIsOptional(t) && t->inner && TypeIsString(&b->m_registry, t->inner->name))
    {
        TypeDesc td = StringFatDesc(b);
        TypeDescSetOptional(&td, true);
        return td;
    }

    // String (or alias): fat {ptr, len, cap}, empty is {null, 0, 0}.
    if (TypeIsString(&b->m_registry, t->name))
    {
        return StringFatDesc(b);
    }

    // CString (or alias): a single borrowed `const char*` to `.rodata`.
    if (TypeIsCString(&b->m_registry, t->name))
    {
        TypeDesc td = {0};
        td.type = b->m_ptrTy;
        TypeDescSetCString(&td, true);
        return td;
    }

    if (TypeNameIsOwning(t))
    {
        TypeDesc td = TypeDescMake(b->m_ptrTy, 0, NULL);
        const TypeName* boxInner = TypeNameBoxInner(t);

        TypeDescSetBox(&td, true);
        TypeDescSetOptional(&td, TypeNameIsOptional(t)); // `T?` rebinds whole slots on assign.

        /* `^T?` (a box over `T?`, or an optional over `^T`) is ONE heap cell
           holding a T, exactly like `T?`: the box-up sites construct a T into
           the cell, so the cell's inner type (its size, deref and drop) must
           be T, not another box pointer. */
        while (boxInner && (TypeNameIsBox(boxInner) || TypeNameIsOptional(boxInner)) && boxInner->inner)
        {
            TypeDescSetOptional(&td, TypeDescIsOptional(&td) || TypeNameIsOptional(boxInner));
            boxInner = boxInner->inner;
        }

        if (boxInner)
        {
            td.boxInner = boxInner;
        }

        return td;
    }

    if (b->m_diag)
    {
        DiagErrorFmt(b->m_diag, t->range, "unknown type '%s'", t->name);
    }

    return TypeDescMake(b->m_ptrTy, 0, NULL);
}

LLVMValueRef ZeroOf(TypeDesc typeDesc)
{
    return LLVMConstNull(typeDesc.type);
}

// Marks "computed, no defaults" (and "being computed") in m_structDefaults.
static char s_noStructDefault;

/* A field default folded by sema, as a constant of the field's own type. */
static LLVMValueRef FoldedDefaultConstant(TypeDesc typeDesc, const FieldDefaultValue* value)
{
    if (TypeDescIsSimdVector(&typeDesc))
    {
        LLVMTypeRef laneTy = LLVMGetElementType(typeDesc.type);
        unsigned laneCount = LLVMGetVectorSize(typeDesc.type);
        LLVMValueRef lanes[4];

        for (unsigned lane = 0; lane < laneCount && lane < 4; lane++)
        {
            lanes[lane] = LLVMConstReal(laneTy, lane < value->laneCount ? value->floatLanes[lane] : 0.0);
        }

        return LLVMConstVector(lanes, (int)(laneCount < 4 ? laneCount : 4));
    }

    if (TypeDescIsFloat(&typeDesc))
    {
        return LLVMConstReal(typeDesc.type, value->floatLanes[0]);
    }

    return ConstIntTrunc(typeDesc.type, (ConstUInt)value->intValue);
}

/* `structName` with its field defaults applied, or NULL when that is all-zero. */
static LLVMValueRef StructDefaultConstant(Builder* b, const char* structName)
{
    void* cached = StrMapGet(&b->m_structDefaults, structName);

    if (cached)
    {
        return cached == &s_noStructDefault ? NULL : (LLVMValueRef)cached;
    }

    StrMapPut(&b->m_structDefaults, structName, &s_noStructDefault);

    const StructType* st = TypeRegistryFind(&b->m_registry, structName);
    LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, structName);

    if (!st || st->opaque || st->isTypeAlias || st->isUnion || !st->hasLayout || !structTy || st->fields.count == 0)
    {
        return NULL;
    }

    unsigned memberCount = LLVMCountStructElementTypes(structTy);
    LLVMValueRef* members = (LLVMValueRef*)arena_alloc(b->m_arena, memberCount * sizeof(LLVMValueRef));

    for (unsigned i = 0; i < memberCount; i++)
    {
        members[i] = LLVMConstNull(LLVMStructGetTypeAtIndex(structTy, i));
    }

    bool hasDefault = false;

    for (size_t i = 0; i < st->fields.count; i++)
    {
        const FieldDecl* field = (const FieldDecl*)VecGet(&st->fields, i);
        LLVMValueRef value = field->folded.isSet ? FoldedDefaultConstant(Resolve(b, &field->type), &field->folded)
                                                 : TypeDefaultConstant(b, &field->type);
        FieldIndex member = PhysicalFieldIndex(st, (int)i);

        if (value && member < memberCount)
        {
            members[member] = value;
            hasDefault = true;
        }
    }

    if (!hasDefault)
    {
        return NULL;
    }

    LLVMValueRef constant = LLVMConstNamedStruct(structTy, members, memberCount);
    StrMapPut(&b->m_structDefaults, structName, (void*)constant);

    return constant;
}

/* The default value of `type` when field defaults make it differ from all-zero (a struct with
   defaults, directly or nested, or a fixed array of one), else NULL. */
static LLVMValueRef TypeDefaultConstant(Builder* b, const TypeName* type)
{
    if (!type || !type->name || TypeNameIsBox(type) || TypeNameIsOptional(type))
    {
        return NULL;
    }

    if (TypeNameHasArrayShape(type))
    {
        if (type->length < 1)
        {
            return NULL;
        }

        LLVMValueRef element = TypeDefaultConstant(b, type->elem);

        if (!element)
        {
            return NULL;
        }

        LLVMValueRef* elements = (LLVMValueRef*)arena_alloc(b->m_arena, (size_t)type->length * sizeof(LLVMValueRef));

        for (ArrayLength i = 0; i < type->length; i++)
        {
            elements[i] = element;
        }

        return LLVMConstArray(LLVMTypeOf(element), elements, (unsigned)type->length);
    }

    const char* leaf = TypeRegistryResolveAlias(&b->m_registry, type->name);

    return leaf ? StructDefaultConstant(b, leaf) : NULL;
}

/* What an uninitialized binding of `type` starts as: its field defaults, else zero. */
LLVMValueRef DefaultValueOf(Builder* b, const TypeName* type, TypeDesc typeDesc)
{
    LLVMValueRef defaults = TypeDefaultConstant(b, type);

    return defaults ? defaults : ZeroOf(typeDesc);
}

Value ZeroInt(Builder* b)
{
    return ValueMake(LLVMConstNull(I32Ty(b)), TypeDescMake(I32Ty(b), 0, NULL));
}

Value Coerce(Builder* b, Value value, TypeDesc target)
{
    if (TypeDescIsBox(&value.typeDesc) && !TypeDescIsBox(&target))
    {
        value = DerefBoxValue(b, value);
    }

    if (!value.typeDesc.type || !target.type || value.typeDesc.type == target.type)
    {
        return value;
    }

    /* Handle <-> integer (sema allows these only as explicit casts): a handle is its address. */
    bool valueIsHandle = value.typeDesc.structTypeName && IsHandleType(&b->m_registry, value.typeDesc.structTypeName);
    bool targetIsHandle = target.structTypeName && IsHandleType(&b->m_registry, target.structTypeName);

    if (valueIsHandle && LLVMGetTypeKind(target.type) == LLVMIntegerTypeKind)
    {
        return ValueMake(LLVMBuildPtrToInt(b->m_builder, value.value, target.type, "p2i"), target);
    }

    if (targetIsHandle && LLVMGetTypeKind(value.typeDesc.type) == LLVMIntegerTypeKind)
    {
        bool srcIsBool = value.typeDesc.type == I1Ty(b);
        LLVMValueRef wide = LLVMBuildIntCast2(b->m_builder, value.value, I64Ty(b),
                                              !TypeDescIsUnsigned(&value.typeDesc) && !srcIsBool, "i64");
        return ValueMake(LLVMBuildIntToPtr(b->m_builder, wide, b->m_ptrTy, "i2p"), target);
    }

    if (value.typeDesc.structTypeName || target.structTypeName)
    {
        return value;
    }

    /* `cstring` <-> `string`: copy in either direction at the use site.
       cstring -> string heap-copies the NUL-terminated bytes into a fresh
       owned fat; string -> cstring borrows the fat's data pointer (the NUL
       invariant holds; an empty {null, 0} becomes the static empty buffer). */
    if (TypeDescIsCString(&value.typeDesc) && TypeDescIsString(&target))
    {
        return ValueMake(BuildOwnedStringFromCStr(b, value.value), target);
    }

    if (TypeDescIsString(&value.typeDesc) && TypeDescIsCString(&target))
    {
        return ValueMake(BuildExternStringPtr(b, value), target);
    }

    /* Conversion to bool is a (non)zero test, not a bit-truncation. */
    if (target.type == I1Ty(b))
    {
        LLVMValueRef zero = LLVMConstNull(value.typeDesc.type);
        LLVMValueRef cmp = TypeDescIsFloat(&value.typeDesc)
                               ? LLVMBuildFCmp(b->m_builder, LLVMRealUNE, value.value, zero, "tobool")
                               : LLVMBuildICmp(b->m_builder, LLVMIntNE, value.value, zero, "tobool");
        return ValueMake(cmp, target);
    }

    LLVMValueRef r = NULL;

    if (!TypeDescIsFloat(&value.typeDesc) && TypeDescIsFloat(&target))
    {
        r = TypeDescIsUnsigned(&value.typeDesc) ? LLVMBuildUIToFP(b->m_builder, value.value, target.type, "c")
                                                : LLVMBuildSIToFP(b->m_builder, value.value, target.type, "c");
    }
    else if (TypeDescIsFloat(&value.typeDesc) && !TypeDescIsFloat(&target))
    {
        r = BuildFloatToIntSaturating(b, value.value, target.type, TypeDescIsUnsigned(&target));
    }
    else if (!TypeDescIsFloat(&value.typeDesc) && !TypeDescIsFloat(&target) && !TypeDescIsVoid(&target))
    {
        /* Widen per the SOURCE's signedness: an unsigned i8/i16/i32 (byte,
           ushort, uint) must zero-extend, a signed one sign-extend. The
           target's signedness only matters for truncation, where the flag
           is irrelevant. */
        bool srcIsBool = value.typeDesc.type == I1Ty(b);
        r = LLVMBuildIntCast2(b->m_builder, value.value, target.type,
                              !TypeDescIsUnsigned(&value.typeDesc) && !srcIsBool, "c");
    }
    else if (TypeDescIsFloat(&value.typeDesc) && TypeDescIsFloat(&target))
    {
        /* float <-> double: widen (fpext) or narrow (fptrunc). SIMD vectors
           are float-typed but never convert element-wise here - they fall
           through to `return value` unchanged. */
        LLVMTypeRef f = LLVMFloatTypeInContext(b->m_ctx);
        LLVMTypeRef d = LLVMDoubleTypeInContext(b->m_ctx);

        if (value.typeDesc.type == f && target.type == d)
        {
            r = LLVMBuildFPExt(b->m_builder, value.value, d, "c");
        }
        else if (value.typeDesc.type == d && target.type == f)
        {
            r = LLVMBuildFPTrunc(b->m_builder, value.value, f, "c");
        }
        else
        {
            return value;
        }
    }
    else
    {
        return value;
    }

    return ValueMake(r, target);
}

/* T's descriptor for `extern<T>` calls: {nameHash, layoutHash, size, alignment} then the
   NUL-terminated name, as StrataTypeDesc in strata.h. */
LLVMValueRef TypeDescConstant(Builder* b, const char* structName)
{
    LLVMValueRef existing = (LLVMValueRef)StrMapGet(&b->m_typeDescs, structName);

    if (existing)
    {
        return existing;
    }

    const StructType* st = TypeRegistryFind(&b->m_registry, structName);
    size_t nameLength = strlen(structName);

    LLVMValueRef name = LLVMConstStringInContext(b->m_ctx, structName, (unsigned)nameLength, 0);
    LLVMTypeRef memberTypes[5] = {I64Ty(b), I64Ty(b), I32Ty(b), I32Ty(b), LLVMTypeOf(name)};
    LLVMValueRef members[5] = {
        LLVMConstInt(I64Ty(b), TypeMetadataNameHash(structName), 0),
        LLVMConstInt(I64Ty(b), st ? TypeMetadataLayoutHash(&b->m_registry, b->m_module, st) : 0, 0),
        LLVMConstInt(I32Ty(b), st ? (unsigned long long)st->sizeBytes : 0, 0),
        LLVMConstInt(I32Ty(b), st ? (unsigned long long)st->alignBytes : 1, 0),
        name,
    };

    LLVMTypeRef descTy = LLVMStructTypeInContext(b->m_ctx, memberTypes, 5, 0);
    LLVMValueRef desc = LLVMAddGlobal(b->m_mod, descTy, arena_format(b->m_arena, ".typedesc.%s", structName));
    LLVMSetInitializer(desc, LLVMConstStructInContext(b->m_ctx, members, 5, 0));
    LLVMSetLinkage(desc, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(desc, 1);
    LLVMSetAlignment(desc, 8);

    StrMapPut(&b->m_typeDescs, structName, (void*)desc);

    return desc;
}
