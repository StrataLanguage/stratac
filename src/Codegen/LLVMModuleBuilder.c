#include "LLVMModuleBuilder.h"
#include "TypeRegistry.h"
#include "TypeUtil.h"
#include "AST/AST.h"
#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    LLVMTypeRef type;
    bool isFloat;
    bool isUnsigned;
    bool isVoid;
    const char* structTypeName;
    bool isBox;
    const char* boxInner;
    bool isArray;
    const char* arrayInner;  /* element type name (arena-owned) */
} TypeDesc;

typedef struct {
    LLVMValueRef value;
    TypeDesc typeDesc;
} Value;

typedef struct {
    LLVMValueRef function;
    LLVMTypeRef type;
    TypeDesc returnType;
    bool* paramByPtr;
    size_t paramByPtrCount;
} FuncInfo;

typedef struct {
    bool valid;
    LLVMValueRef ptr;
    TypeDesc typeDesc;
} LValue;

/* An owning local: a slot that must be dropped (freed) on scope exit.
   Carries its type so the drop can dispatch between box/string and array
   (which must free its backing buffer, and recursively drop owning elems). */
typedef struct {
    LLVMValueRef slot;
    TypeDesc td;
} OwnLocal;

typedef struct {
    LLVMBasicBlockRef cont;
    LLVMBasicBlockRef end;
} Loop;

typedef struct {
    DiagnosticEngine* m_diag;
    LLVMContextRef m_ctx;
    LLVMModuleRef m_mod;
    LLVMBuilderRef m_builder;
    LLVMTypeRef m_ptrTy;
    TypeRegistry m_registry;
    StrMap m_structTypes;
    StrMap m_funcs;
    StrMap m_symbols;
    StrMap m_globals;
    StrMap m_externSlots;
    Vec m_externNames;
    TypeDesc m_curRet;
    bool m_terminated;
    bool m_jitMode;
    LLVMValueRef m_curFn;
    LLVMBasicBlockRef m_entryBlock;
    LLVMValueRef m_entryAllocaPt;
    Vec m_loops;
    Vec m_owningLocals;
    LLVMTypeRef m_arrayType;   /* cached {ptr, i64} fat struct for T[] */
    LLVMValueRef m_allocFn;
    LLVMTypeRef m_allocFnType;
    LLVMValueRef m_freeFn;
    LLVMTypeRef m_freeFnType;
    Arena* m_arena;
    int m_strLitCount;
} Builder;

static TypeDesc TypeDescMake(LLVMTypeRef type, bool isFloat, bool isUnsigned, bool isVoid, const char* structTypeName)
{
    TypeDesc td = {0};
    td.type = type;
    td.isFloat = isFloat;
    td.isUnsigned = isUnsigned;
    td.isVoid = isVoid;
    td.structTypeName = structTypeName;

    return td;
}

static Value ValueMake(LLVMValueRef value, TypeDesc typeDesc)
{
    Value v = {0};
    v.value = value;
    v.typeDesc = typeDesc;

    return v;
}

static TypeName MakeTypeName(char* name)
{
    TypeName tn = {0};
    tn.name = name;

    return tn;
}

static LLVMTypeRef ScalarLlvmType(LLVMContextRef ctx, const MappedType* t)
{
    if (t->isVoid)
    {
        return LLVMVoidTypeInContext(ctx);
    }

    LLVMTypeRef elem = NULL;

    if (strcmp(t->elemIr, "i1") == 0)
    {
        elem = LLVMInt1TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i8") == 0)
    {
        elem = LLVMInt8TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i16") == 0)
    {
        elem = LLVMInt16TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i32") == 0)
    {
        elem = LLVMInt32TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "i64") == 0)
    {
        elem = LLVMInt64TypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "float") == 0)
    {
        elem = LLVMFloatTypeInContext(ctx);
    }
    else if (strcmp(t->elemIr, "double") == 0)
    {
        elem = LLVMDoubleTypeInContext(ctx);
    }
    else
    {
        elem = LLVMInt32TypeInContext(ctx);
    }

    if (MappedTypeIsVector(t))
    {
        return LLVMVectorType(elem, (unsigned)t->vec);
    }

    return elem;
}

static LLVMIntPredicate PredNameToPredicate(const char* p, bool uns)
{
    if (strcmp(p, "eq") == 0)
    {
        return LLVMIntEQ;
    }

    if (strcmp(p, "ne") == 0)
    {
        return LLVMIntNE;
    }

    if (strcmp(p, "lt") == 0)
    {
        return uns ? LLVMIntULT : LLVMIntSLT;
    }

    if (strcmp(p, "le") == 0)
    {
        return uns ? LLVMIntULE : LLVMIntSLE;
    }

    if (strcmp(p, "gt") == 0)
    {
        return uns ? LLVMIntUGT : LLVMIntSGT;
    }

    if (strcmp(p, "ge") == 0)
    {
        return uns ? LLVMIntUGE : LLVMIntSGE;
    }

    return LLVMIntEQ;
}

static LLVMRealPredicate RealPredNameToPredicate(const char* p)
{
    if (strcmp(p, "oeq") == 0) return LLVMRealOEQ;
    if (strcmp(p, "ogt") == 0) return LLVMRealOGT;
    if (strcmp(p, "oge") == 0) return LLVMRealOGE;
    if (strcmp(p, "olt") == 0) return LLVMRealOLT;
    if (strcmp(p, "ole") == 0) return LLVMRealOLE;
    if (strcmp(p, "one") == 0) return LLVMRealONE;

    return LLVMRealOEQ;
}

static LLVMValueRef IcmpByName(LLVMBuilderRef builder, const char* pred, bool isUnsigned, LLVMValueRef l, LLVMValueRef r)
{
    return LLVMBuildICmp(builder, PredNameToPredicate(pred, isUnsigned), l, r, "cmp");
}

static LLVMValueRef FcmpByName(LLVMBuilderRef builder, const char* pred, LLVMValueRef l, LLVMValueRef r)
{
    return LLVMBuildFCmp(builder, RealPredNameToPredicate(pred), l, r, "cmp");
}

static LLVMTypeRef I32Ty(Builder* b)
{
    return LLVMInt32TypeInContext(b->m_ctx);
}

static LLVMTypeRef I64Ty(Builder* b)
{
    return LLVMInt64TypeInContext(b->m_ctx);
}

static LLVMTypeRef I1Ty(Builder* b)
{
    return LLVMInt1TypeInContext(b->m_ctx);
}

/* The fat {ptr, i64} representation shared by every T[]: a data pointer and
   a u64 length. Cached so all arrays share one struct type. */
static LLVMValueRef IdxConst(Builder* b, unsigned i);  /* forward decl */

static LLVMTypeRef ArrayStructType(Builder* b)
{
    if (!b->m_arrayType)
    {
        LLVMTypeRef fields[2] = { b->m_ptrTy, I64Ty(b) };
        b->m_arrayType = LLVMStructTypeInContext(b->m_ctx, fields, 2, 0);
    }

    return b->m_arrayType;
}

/* GEP helpers for the two array-slot fields: 0 = data ptr, 1 = length. */
static LLVMValueRef ArrayDataPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = { IdxConst(b, 0), IdxConst(b, 0) };
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "aptr");
}

static LLVMValueRef ArrayLenPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = { IdxConst(b, 0), IdxConst(b, 1) };
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "alen");
}

static LLVMBasicBlockRef NewBb(Builder* b, const char* name)
{
    return LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, name);
}

static LLVMValueRef EntryAlloca(Builder* b, LLVMTypeRef type, const char* name)
{
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(b->m_builder);

    if (b->m_entryAllocaPt)
    {
        LLVMPositionBuilderBefore(b->m_builder, b->m_entryAllocaPt);
    }
    else
    {
        LLVMPositionBuilderAtEnd(b->m_builder, b->m_entryBlock);
    }

    LLVMValueRef slot = LLVMBuildAlloca(b->m_builder, type, name);
    b->m_entryAllocaPt = slot;

    LLVMPositionBuilderAtEnd(b->m_builder, cur);
    return slot;
}

static void PositionAtEnd(Builder* b, LLVMBasicBlockRef bb)
{
    LLVMPositionBuilderAtEnd(b->m_builder, bb);
    b->m_terminated = false;
}

static void Br(Builder* b, LLVMBasicBlockRef dest)
{
    if (!b->m_terminated)
    {
        LLVMBuildBr(b->m_builder, dest);
        b->m_terminated = true;
    }
}

static LLVMValueRef IdxConst(Builder* b, unsigned i)
{
    return LLVMConstInt(I32Ty(b), i, 1);
}

static TypeDesc Resolve(Builder* b, const TypeName* t);
static Value ZeroInt(Builder* b);
static Value Coerce(Builder* b, Value value, TypeDesc target);
static LLVMValueRef ToI1(Builder* b, Value v);
static void EmitStmt(Builder* b, Node* n);
static Value EmitExpr(Builder* b, Node* n);
static Value EmitIdent(Builder* b, IdentExpr* n);
static LValue EmitLValue(Builder* b, Node* n);
static Value EmitMember(Builder* b, MemberExpr* n);
static Value EmitIndex(Builder* b, IndexExpr* n);
static Value EmitArrayInit(Builder* b, ArrayInitExpr* n);
static LLVMValueRef AsI64Index(Builder* b, Value v);
static Value EmitUnary(Builder* b, UnaryExpr* n);
static Value EmitBinary(Builder* b, BinaryExpr* n);
static Value EmitAssign(Builder* b, AssignExpr* n);
static Value EmitCall(Builder* b, CallExpr* n);
static Value EmitStructInit(Builder* b, StructInitExpr* n);
static LLVMValueRef ArgAddress(Builder* b, Node* arg);
static void DeclareFunction(Builder* b, const FunctionDecl* f);
static void DefineFunction(Builder* b, const FunctionDecl* f);

static TypeDesc Resolve(Builder* b, const TypeName* t)
{
    MappedType mapped = MapType(t);

    if (mapped.valid)
    {
        return TypeDescMake(ScalarLlvmType(b->m_ctx, &mapped), mapped.isFloat, mapped.isUnsigned, mapped.isVoid, NULL);
    }

    LLVMTypeRef found = (LLVMTypeRef)StrMapGet(&b->m_structTypes, t->name);

    if (found)
    {
        return TypeDescMake(found, false, false, false, t->name);
    }

    Str arrInner = ArrayInnerStr(t->name);

    if (arrInner.data)
    {
        /* T[]: a fat {ptr, u64} struct (data pointer + length). The slot is
           always addressable (alloca / by-ref), so Resolve yields the struct
           type with the element type tagged on the side. */
        TypeDesc td = {0};
        td.type = ArrayStructType(b);
        td.isArray = true;
        td.arrayInner = arena_strndup(b->m_arena, arrInner.data, arrInner.len);

        return td;
    }

    if (IsOwningType(t->name))
    {
        TypeDesc td = TypeDescMake(b->m_ptrTy, false, false, false, NULL);
        Str _inner = OwningInnerStr(t->name);

        if (_inner.data)
        {
            td.isBox = true;
            td.boxInner = arena_strndup(b->m_arena, _inner.data, _inner.len);
        }
        else if (strcmp(t->name, "string") == 0)
        {
            td.isBox = true;
        }

        return td;
    }

    if (b->m_diag)
    {
        DiagErrorFmt(b->m_diag, t->range, "unknown type '%s'", t->name);
    }

    return TypeDescMake(b->m_ptrTy, false, false, false, NULL);
}

static LLVMValueRef ZeroOf(TypeDesc typeDesc)
{
    return LLVMConstNull(typeDesc.type);
}

static Value ZeroInt(Builder* b)
{
    return ValueMake(LLVMConstNull(I32Ty(b)), TypeDescMake(I32Ty(b), false, false, false, NULL));
}

/* Reads a box value's inner value through the pointer, leaving the box
   itself alone (not moved, not freed). */
static Value DerefBoxValue(Builder* b, Value value)
{
    if (!value.typeDesc.isBox)
    {
        return value;
    }

    if (!value.typeDesc.boxInner)
    {
        return value;
    }

    TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)value.typeDesc.boxInner});
    LLVMValueRef loaded = LLVMBuildLoad2(b->m_builder, innerTd.type, value.value, "boxval");

    return ValueMake(loaded, innerTd);
}

static Value Coerce(Builder* b, Value value, TypeDesc target)
{
    if (value.typeDesc.isBox && !target.isBox)
    {
        value = DerefBoxValue(b, value);
    }

    if (!value.typeDesc.type || !target.type || value.typeDesc.type == target.type)
    {
        return value;
    }

    if (value.typeDesc.structTypeName || target.structTypeName)
    {
        return value;
    }

    /* Conversion to bool is a (non)zero test, not a bit-truncation. */
    if (target.type == I1Ty(b))
    {
        LLVMValueRef zero = LLVMConstNull(value.typeDesc.type);
        LLVMValueRef cmp = value.typeDesc.isFloat
            ? LLVMBuildFCmp(b->m_builder, LLVMRealUNE, value.value, zero, "tobool")
            : LLVMBuildICmp(b->m_builder, LLVMIntNE, value.value, zero, "tobool");
        return ValueMake(cmp, target);
    }

    LLVMValueRef r = NULL;

    if (!value.typeDesc.isFloat && target.isFloat)
    {
        r = value.typeDesc.isUnsigned
            ? LLVMBuildUIToFP(b->m_builder, value.value, target.type, "c")
            : LLVMBuildSIToFP(b->m_builder, value.value, target.type, "c");
    }
    else if (value.typeDesc.isFloat && !target.isFloat)
    {
        r = target.isUnsigned
            ? LLVMBuildFPToUI(b->m_builder, value.value, target.type, "c")
            : LLVMBuildFPToSI(b->m_builder, value.value, target.type, "c");
    }
    else if (!value.typeDesc.isFloat && !target.isFloat && !target.isVoid)
    {
        bool srcIsBool = value.typeDesc.type == I1Ty(b);
        r = LLVMBuildIntCast2(b->m_builder, value.value, target.type, !target.isUnsigned && !srcIsBool, "c");
    }
    else
    {
        return value;
    }

    return ValueMake(r, target);
}

/* If value is box<T> but the target context expects T, load the pointee. */
static Value UnboxIfBox(Builder* b, Value value, TypeDesc target)
{
    if (value.typeDesc.isBox && !target.isBox && value.typeDesc.boxInner)
    {
        TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)value.typeDesc.boxInner});
        LLVMValueRef loaded = LLVMBuildLoad2(b->m_builder, innerTd.type, value.value, "unbox");
        return ValueMake(loaded, innerTd);
    }

    return value;
}

static LLVMValueRef StrataAllocFn(Builder* b)
{
    if (!b->m_allocFn)
    {
        LLVMTypeRef params[1] = { I64Ty(b) };
        b->m_allocFnType = LLVMFunctionType(b->m_ptrTy, params, 1, 0);
        b->m_allocFn = LLVMAddFunction(b->m_mod, "strata_alloc", b->m_allocFnType);
    }

    return b->m_allocFn;
}

static LLVMValueRef StrataFreeFn(Builder* b)
{
    if (!b->m_freeFn)
    {
        LLVMTypeRef params[1] = { b->m_ptrTy };
        b->m_freeFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_freeFn = LLVMAddFunction(b->m_mod, "strata_free", b->m_freeFnType);
    }

    return b->m_freeFn;
}

static LLVMValueRef SizeOfConst(Builder* b, LLVMTypeRef ty)
{
    LLVMValueRef idx[1] = { LLVMConstInt(I64Ty(b), 1, 0) };
    LLVMValueRef gep = LLVMConstGEP2(ty, LLVMConstNull(b->m_ptrTy), idx, 1);

    return LLVMConstPtrToInt(gep, I64Ty(b));
}

/* Drops (frees) the owning value held in 'slot', typed 'td'.
   - box<T> / string: 'slot' is the address of a pointer; free it.
   - T[]: 'slot' is the address of a {ptr, u64} struct; free the backing
     buffer, and when the element type is itself owning (box/string/array),
     drop each element first via a loop. */
static void EmitDropOne(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    if (td.isArray)
    {
        LLVMValueRef dataPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, slot), "adrop");

        if (td.arrayInner && IsOwningType(td.arrayInner))
        {
            LLVMValueRef lenVal = LLVMBuildLoad2(b->m_builder, I64Ty(b), ArrayLenPtr(b, slot), "adlen");
            TypeDesc elemTd = Resolve(b, &(TypeName){.name = (char*)td.arrayInner});
            LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

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
            LLVMValueRef epIdx[1] = { i };
            LLVMValueRef elemPtr = LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, epIdx, 1, "aelem");
            EmitDropOne(b, elemPtr, elemTd);
            LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "next");
            LLVMBuildStore(b->m_builder, next, iSlot);
            Br(b, cond);

            PositionAtEnd(b, end);
        }

        LLVMValueRef args[1] = { dataPtr };
        StrataFreeFn(b);
        LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, args, 1, "");
        LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), ArrayDataPtr(b, slot));
        LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), ArrayLenPtr(b, slot));

        return;
    }

    /* box / string */
    LLVMValueRef ptr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slot, "box");
    LLVMValueRef args[1] = { ptr };
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, args, 1, "");
    LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), slot);
}

static void EmitDrops(Builder* b, size_t fromIndex)
{
    for (size_t i = fromIndex; i < b->m_owningLocals.count; i++)
    {
        OwnLocal* ol = (OwnLocal*)VecGet(&b->m_owningLocals, i);
        EmitDropOne(b, ol->slot, ol->td);
    }
}

static void DeclareFunction(Builder* b, const FunctionDecl* f)
{
    FuncInfo* info = (FuncInfo*)arena_alloc(b->m_arena, sizeof(FuncInfo));

    info->returnType = Resolve(b, &f->returnType);

    size_t pcount = f->params.count;
    info->paramByPtr = (bool*)arena_alloc(b->m_arena, pcount * sizeof(bool));
    info->paramByPtrCount = pcount;

    LLVMTypeRef* params = NULL;

    if (pcount > 0)
    {
        params = (LLVMTypeRef*)arena_alloc(b->m_arena, pcount * sizeof(LLVMTypeRef));
    }

    for (size_t i = 0; i < pcount; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);

        bool structVal = TypeRegistryIsUserType(&b->m_registry, p->type.name) && !TypeRegistryIsOpaque(&b->m_registry, p->type.name);

        bool byPtr = p->mod != ModNone || structVal || IsOwningType(p->type.name);

        /* For extern functions, a string param passes the data pointer
           (const char*) directly, not a pointer to the owner slot.  The
           host can read the string without taking ownership. */
        if (byPtr && f->isExtern && strcmp(p->type.name, "string") == 0)
        {
            byPtr = false;
        }

        info->paramByPtr[i] = byPtr;

        params[i] = byPtr ? b->m_ptrTy : Resolve(b, &p->type).type;
    }

    info->type = LLVMFunctionType(info->returnType.type, params, (unsigned)pcount, 0);

    if (b->m_jitMode && f->isExtern)
    {
        char* slotName = arena_format(b->m_arena, "__strata_ext_%s", f->name);

        LLVMValueRef slot = LLVMAddGlobal(b->m_mod, b->m_ptrTy, slotName);
        LLVMSetInitializer(slot, LLVMConstNull(b->m_ptrTy));

        StrMapPut(&b->m_externSlots, f->name, (void*)slot);

        info->function = NULL;
    }
    else
    {
        info->function = LLVMAddFunction(b->m_mod, f->mangledName, info->type);
    }

    StrMapPut(&b->m_funcs, f->mangledName, info);
}

static void DefineFunction(Builder* b, const FunctionDecl* f)
{
    if (!f->body)
    {
        return;
    }

    StrMapClear(&b->m_symbols);
    b->m_terminated = false;
    b->m_curRet = Resolve(b, &f->returnType);
    b->m_loops.count = 0;
    b->m_owningLocals.count = 0;

    FuncInfo* info = (FuncInfo*)StrMapGet(&b->m_funcs, f->mangledName);
    b->m_curFn = info ? info->function : NULL;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "entry");
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;
    LLVMPositionBuilderAtEnd(b->m_builder, entry);

    for (size_t i = 0; i < f->params.count; i++)
    {
        ParamDecl* p = (ParamDecl*)VecGet(&f->params, i);
        TypeDesc typeDesc = Resolve(b, &p->type);

        bool structVal = TypeRegistryIsUserType(&b->m_registry, p->type.name) && !TypeRegistryIsOpaque(&b->m_registry, p->type.name);
        bool boxParam = IsOwningType(p->type.name);

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));

        if (p->mod != ModNone || structVal || boxParam)
        {
            sym->value = LLVMGetParam(b->m_curFn, (unsigned)i);
            sym->typeDesc = typeDesc;

            /* An owned (non-ref) box/array parameter is consumed: freed at return. */
            if (boxParam && p->mod == ModNone)
            {
                OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
                ol->slot = sym->value;
                ol->td = typeDesc;
                VecPush(&b->m_owningLocals, ol);
            }
        }
        else
        {
            LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "arg");
            LLVMBuildStore(b->m_builder, LLVMGetParam(b->m_curFn, (unsigned)i), slot);

            sym->value = slot;
            sym->typeDesc = typeDesc;
        }

        StrMapPut(&b->m_symbols, p->name, sym);
    }

    Block* blk = (Block*)f->body;

    for (size_t i = 0; i < blk->statements.count; i++)
    {
        Node* s = (Node*)VecGet(&blk->statements, i);
        EmitStmt(b, s);

        if (b->m_terminated)
        {
            break;
        }
    }

    if (!b->m_terminated)
    {
        EmitDrops(b, 0);

        if (b->m_curRet.isVoid)
        {
            LLVMBuildRetVoid(b->m_builder);
        }
        else
        {
            LLVMBuildRet(b->m_builder, ZeroOf(b->m_curRet));
        }
    }
}

static LLVMValueRef ToI1(Builder* b, Value v)
{
    if (v.typeDesc.type == I1Ty(b))
    {
        return v.value;
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

    if (!sym)
    {
        sym = (Value*)StrMapGet(&b->m_globals, n->name);
    }

    if (!sym)
    {
        if (b->m_diag)
        {
            DiagErrorFmt(b->m_diag, n->base.range, "unknown identifier '%s'", n->name);
        }

        return ZeroInt(b);
    }

    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, sym->typeDesc.type, sym->value, "id");

    return ValueMake(v, sym->typeDesc);
}

/* Unwraps casts to find the lvalue being moved (identifier or field chain),
   for nulling via EmitLValue. */
static Node* MovableBoxSourceNode(Node* n)
{
    while (n && n->kind == NodeCast)
    {
        n = ((CastExpr*)n)->operand;
    }

    return (n && (n->kind == NodeIdent || n->kind == NodeMember)) ? n : NULL;
}

/* Nulls the owning binding (box/string/array ident or member chain) behind a
   moved value, so the source is no longer responsible for freeing it. */
static void NullMovedSource(Builder* b, Node* n)
{
    Node* moved = MovableBoxSourceNode(n);

    if (!moved)
    {
        return;
    }

    LValue src = EmitLValue(b, moved);

    if (src.valid)
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
    }
}

static LValue EmitLValue(Builder* b, Node* n)
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

        if (!sym)
        {
            sym = (Value*)StrMapGet(&b->m_globals, id->name);
        }

        if (!sym)
        {
            return none;
        }

        none.valid = true;
        none.ptr = sym->value;
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

        /* array.length — addressable only as a read; returns the u64 slot. */
        if (base.typeDesc.isArray && strcmp(m->member, "length") == 0)
        {
            none.valid = true;
            none.ptr = ArrayLenPtr(b, base.ptr);
            none.typeDesc = TypeDescMake(I64Ty(b), false, true, false, NULL);  /* ulong */
            return none;
        }

        LLVMValueRef structPtr;
        LLVMTypeRef structTy;
        const char* structName;

        if (base.typeDesc.isBox)
        {
            structPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, base.ptr, "box");
            structName = base.typeDesc.boxInner;
            structTy = Resolve(b, &(TypeName){.name = (char*)base.typeDesc.boxInner}).type;
        }
        else if (base.typeDesc.structTypeName)
        {
            structPtr = base.ptr;
            structName = base.typeDesc.structTypeName;
            structTy = base.typeDesc.type;
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

        LLVMValueRef idxs[2] = {0};
        idxs[0] = IdxConst(b, 0);
        idxs[1] = IdxConst(b, (unsigned)idx);

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

        if (!base.valid || !base.typeDesc.isArray)
        {
            return none;
        }

        LLVMValueRef dataPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, base.ptr), "ap");

        TypeDesc elemTd = Resolve(b, &(TypeName){.name = (char*)base.typeDesc.arrayInner});
        LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

        LLVMValueRef idxArgs[1] = { AsI64Index(b, EmitExpr(b, ix->index)) };
        LLVMValueRef elemAddr = LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, idxArgs, 1, "ai");

        none.valid = true;
        none.ptr = elemAddr;
        none.typeDesc = elemTd;

        return none;
    }

    return none;
}

/* Coerces an integer/float index value to an i64 GEP index. */
static LLVMValueRef AsI64Index(Builder* b, Value v)
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
    LValue lvalue = EmitLValue(b, (Node*)n);

    if (lvalue.valid)
    {
        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, lvalue.typeDesc.type, lvalue.ptr, "m");

        return ValueMake(v, lvalue.typeDesc);
    }

    Value base = EmitExpr(b, n->base_node);

    /* array.length on a non-lvalue array (e.g. MakeArr().length). */
    if (base.typeDesc.isArray && strcmp(n->member, "length") == 0)
    {
        LLVMValueRef len = LLVMBuildExtractValue(b->m_builder, base.value, 1, "len");
        return ValueMake(len, TypeDescMake(I64Ty(b), false, true, false, NULL));
    }

    if (base.typeDesc.isBox && base.typeDesc.boxInner)
    {
        /* A box-typed value with no addressable lvalue (e.g. a call result
           used directly: `MakeThing().field`) - GEP+load through the
           pointer instead of extracting from an aggregate value. */
        LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, base.typeDesc.boxInner);
        int idx = TypeRegistryFieldIndex(&b->m_registry, base.typeDesc.boxInner, n->member);

        if (structTy && idx >= 0)
        {
            const StructType* st = TypeRegistryFind(&b->m_registry, base.typeDesc.boxInner);
            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, (size_t)idx);
            TypeDesc fieldTypeDesc = Resolve(b, &fieldDecl->type);

            LLVMValueRef idxs[2] = { IdxConst(b, 0), IdxConst(b, (unsigned)idx) };
            LLVMValueRef ptr = LLVMBuildGEP2(b->m_builder, structTy, base.value, idxs, 2, "f");
            LLVMValueRef v = LLVMBuildLoad2(b->m_builder, fieldTypeDesc.type, ptr, "m");

            return ValueMake(v, fieldTypeDesc);
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

            LLVMValueRef v = LLVMBuildExtractValue(b->m_builder, base.value, (unsigned)idx, "m");

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

    if (!base.typeDesc.isArray)
    {
        if (b->m_diag)
        {
            DiagErrorFmt(b->m_diag, n->base.range, "indexing a non-array value");
        }

        return ZeroInt(b);
    }

    LLVMValueRef dataPtr = LLVMBuildExtractValue(b->m_builder, base.value, 0, "ap");

    TypeDesc elemTd = Resolve(b, &(TypeName){.name = (char*)base.typeDesc.arrayInner});
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

    LLVMValueRef idxArgs[1] = { AsI64Index(b, EmitExpr(b, n->index)) };
    LLVMValueRef elemAddr = LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, idxArgs, 1, "ai");
    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, elemTy, elemAddr, "el");

    return ValueMake(v, elemTd);
}

static Value EmitUnary(Builder* b, UnaryExpr* n)
{
    Value e = DerefBoxValue(b, EmitExpr(b, n->operand));

    switch (n->op)
    {
    case UnPos:
        return e;

    case UnNeg:
    {
        LLVMValueRef ref = e.typeDesc.isFloat
            ? LLVMBuildFNeg(b->m_builder, e.value, "neg")
            : LLVMBuildNeg(b->m_builder, e.value, "neg");

        return ValueMake(ref, e.typeDesc);
    }

    case UnNot:
    {
        LLVMValueRef ref = LLVMBuildXor(b->m_builder, e.value, LLVMConstInt(I1Ty(b), 1, 0), "not");

        return ValueMake(ref, TypeDescMake(I1Ty(b), false, false, false, NULL));
    }

    case UnBitNot:
    {
        LLVMValueRef ref = LLVMBuildNot(b->m_builder, e.value, "bnot");

        return ValueMake(ref, e.typeDesc);
    }
    }

    return e;
}

static Value EmitBinary(Builder* b, BinaryExpr* n)
{
    if (n->op == BinLogicAnd || n->op == BinLogicOr)
    {
        bool isAnd = (n->op == BinLogicAnd);

        Value l = DerefBoxValue(b, EmitExpr(b, n->lhs));

        LLVMValueRef cond = l.value;
        if (l.typeDesc.type != I1Ty(b))
        {
            if (l.typeDesc.isFloat)
            {
                cond = LLVMBuildFCmp(b->m_builder, LLVMRealONE, l.value,
                                     LLVMConstReal(l.typeDesc.type, 0.0), "tobool");
            }
            else
            {
                cond = LLVMBuildICmp(b->m_builder, LLVMIntNE, l.value,
                                     LLVMConstInt(l.typeDesc.type, 0, 0), "tobool");
            }
        }

        LLVMBasicBlockRef lhsEnd = LLVMGetInsertBlock(b->m_builder);
        LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "logic.rhs");
        LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_curFn, "logic.end");

        if (isAnd)
        {
            LLVMBuildCondBr(b->m_builder, cond, rhsBlock, mergeBlock);
        }
        else
        {
            LLVMBuildCondBr(b->m_builder, cond, mergeBlock, rhsBlock);
        }

        LLVMPositionBuilderAtEnd(b->m_builder, rhsBlock);
        Value r = DerefBoxValue(b, EmitExpr(b, n->rhs));

        LLVMValueRef rhsCond = r.value;
        if (r.typeDesc.type != I1Ty(b))
        {
            if (r.typeDesc.isFloat)
            {
                rhsCond = LLVMBuildFCmp(b->m_builder, LLVMRealONE, r.value,
                                        LLVMConstReal(r.typeDesc.type, 0.0), "tobool");
            }
            else
            {
                rhsCond = LLVMBuildICmp(b->m_builder, LLVMIntNE, r.value,
                                        LLVMConstInt(r.typeDesc.type, 0, 0), "tobool");
            }
        }

        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(b->m_builder);
        LLVMBuildBr(b->m_builder, mergeBlock);

        LLVMPositionBuilderAtEnd(b->m_builder, mergeBlock);
        LLVMValueRef phi = LLVMBuildPhi(b->m_builder, I1Ty(b), "logic");

        LLVMValueRef shortCircuit = isAnd
            ? LLVMConstNull(I1Ty(b))
            : LLVMConstInt(I1Ty(b), 1, 0);

        LLVMBasicBlockRef incomingBlocks[2] = { lhsEnd, rhsEnd };
        LLVMValueRef incomingVals[2] = { shortCircuit, rhsCond };
        LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

        return ValueMake(phi, TypeDescMake(I1Ty(b), false, false, false, NULL));
    }

    Value l = DerefBoxValue(b, EmitExpr(b, n->lhs));
    Value r = DerefBoxValue(b, EmitExpr(b, n->rhs));

    TypeDesc typeDesc = l.typeDesc;

    if (l.typeDesc.isFloat && !r.typeDesc.isFloat)
    {
        r = Coerce(b, r, l.typeDesc);
    }
    else if (!l.typeDesc.isFloat && r.typeDesc.isFloat)
    {
        l = Coerce(b, l, r.typeDesc);

        typeDesc = r.typeDesc;
    }
    else if (!l.typeDesc.isFloat && !r.typeDesc.isFloat
             && l.typeDesc.type != r.typeDesc.type
             && l.typeDesc.type && r.typeDesc.type)
    {
        if (l.typeDesc.type == I64Ty(b))
        {
            r = Coerce(b, r, l.typeDesc);
        }
        else
        {
            l = Coerce(b, l, r.typeDesc);
            typeDesc = r.typeDesc;
        }
    }

    LLVMValueRef out = NULL;
    bool flt = typeDesc.isFloat;

    TypeDesc boolTypeDesc = TypeDescMake(I1Ty(b), false, false, false, NULL);

    /* For comparisons involving pointer types (string, box, handle), convert
       both operands to i64 so that icmp works correctly. */
    if (typeDesc.type == b->m_ptrTy || r.typeDesc.type == b->m_ptrTy)
    {
        switch (n->op)
        {
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
            typeDesc = TypeDescMake(I64Ty(b), false, false, false, NULL);
            flt = false;
            if (l.typeDesc.type == b->m_ptrTy)
            {
                l.value = LLVMBuildPtrToInt(b->m_builder, l.value, I64Ty(b), "picmp");
                l.typeDesc = typeDesc;
            }
            else
            {
                l = Coerce(b, l, typeDesc);
            }
            if (r.typeDesc.type == b->m_ptrTy)
            {
                r.value = LLVMBuildPtrToInt(b->m_builder, r.value, I64Ty(b), "picmp");
                r.typeDesc = typeDesc;
            }
            else
            {
                r = Coerce(b, r, typeDesc);
            }
            break;
        default:
            break;
        }
    }

    switch (n->op)
    {
    case BinAdd:
        out = flt
            ? LLVMBuildFAdd(b->m_builder, l.value, r.value, "add")
            : LLVMBuildAdd(b->m_builder, l.value, r.value, "add");

        return ValueMake(out, typeDesc);

    case BinSub:
        out = flt
            ? LLVMBuildFSub(b->m_builder, l.value, r.value, "sub")
            : LLVMBuildSub(b->m_builder, l.value, r.value, "sub");

        return ValueMake(out, typeDesc);

    case BinMul:
        out = flt
            ? LLVMBuildFMul(b->m_builder, l.value, r.value, "mul")
            : LLVMBuildMul(b->m_builder, l.value, r.value, "mul");

        return ValueMake(out, typeDesc);

    case BinDiv:
        out = flt
            ? LLVMBuildFDiv(b->m_builder, l.value, r.value, "div")
            : (typeDesc.isUnsigned
                ? LLVMBuildUDiv(b->m_builder, l.value, r.value, "div")
                : LLVMBuildSDiv(b->m_builder, l.value, r.value, "div"));

        return ValueMake(out, typeDesc);

    case BinMod:
        out = flt
            ? LLVMBuildFRem(b->m_builder, l.value, r.value, "mod")
            : (typeDesc.isUnsigned
                ? LLVMBuildURem(b->m_builder, l.value, r.value, "mod")
                : LLVMBuildSRem(b->m_builder, l.value, r.value, "mod"));

        return ValueMake(out, typeDesc);

    case BinBitAnd:
        out = LLVMBuildAnd(b->m_builder, l.value, r.value, "and");

        return ValueMake(out, typeDesc);

    case BinBitOr:
        out = LLVMBuildOr(b->m_builder, l.value, r.value, "or");

        return ValueMake(out, typeDesc);

    case BinBitXor:
        out = LLVMBuildXor(b->m_builder, l.value, r.value, "xor");

        return ValueMake(out, typeDesc);

    case BinShl:
        out = LLVMBuildShl(b->m_builder, l.value, r.value, "shl");

        return ValueMake(out, typeDesc);

    case BinShr:
        out = typeDesc.isUnsigned
            ? LLVMBuildLShr(b->m_builder, l.value, r.value, "shr")
            : LLVMBuildAShr(b->m_builder, l.value, r.value, "shr");

        return ValueMake(out, typeDesc);

    case BinEqEq:
        out = flt
            ? FcmpByName(b->m_builder, "oeq", l.value, r.value)
            : IcmpByName(b->m_builder, "eq", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinNotEq:
        out = flt
            ? FcmpByName(b->m_builder, "one", l.value, r.value)
            : IcmpByName(b->m_builder, "ne", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLt:
        out = flt
            ? FcmpByName(b->m_builder, "olt", l.value, r.value)
            : IcmpByName(b->m_builder, "lt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLtEq:
        out = flt
            ? FcmpByName(b->m_builder, "ole", l.value, r.value)
            : IcmpByName(b->m_builder, "le", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGt:
        out = flt
            ? FcmpByName(b->m_builder, "ogt", l.value, r.value)
            : IcmpByName(b->m_builder, "gt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGtEq:
        out = flt
            ? FcmpByName(b->m_builder, "oge", l.value, r.value)
            : IcmpByName(b->m_builder, "ge", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    default:
        return l;
    }
}

static Value EmitAssign(Builder* b, AssignExpr* n)
{
    Value rhs = EmitExpr(b, n->value);

    if (n->target->kind == NodeIdent || n->target->kind == NodeMember || n->target->kind == NodeIndex)
    {
        LValue lvalue = EmitLValue(b, n->target);

        if (lvalue.valid)
        {
            /* Whole-array rebind: free the old buffer, take the new {ptr,len}
               struct, and null a moved source. */
            if (lvalue.typeDesc.isArray && n->op == AssignSet)
            {
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                Node* movedNode = MovableBoxSourceNode(n->value);

                if (movedNode)
                {
                    LValue src = EmitLValue(b, movedNode);

                    if (src.valid && src.typeDesc.isArray)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
                    }
                }

                return ValueMake(rhs.value, lvalue.typeDesc);
            }

            /* `=` rebinds the box only when the value is itself a box (sema
               already required a matching type); any other assignment into
               a box<T> - including `=` with a plain T value, e.g. `x = 5;`
               - mutates its contents instead. */
            bool boxMove = lvalue.typeDesc.isBox && n->op == AssignSet && rhs.typeDesc.isBox;
            bool boxMoveFromLiteral = boxMove && !lvalue.typeDesc.boxInner && n->value->kind == NodeStrLiteral;

            if (boxMove && !boxMoveFromLiteral)
            {
                /* Box move: free the old value, take the new pointer, null the source. */
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                Node* movedSourceNode = MovableBoxSourceNode(n->value);

                if (movedSourceNode)
                {
                    LValue src = EmitLValue(b, movedSourceNode);

                    if (src.valid)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
                    }
                }

                return rhs;
            }

            if (boxMoveFromLiteral)
            {
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                StrLiteral* lit = AsNode(StrLiteral, n->value);
                size_t copyLen = strlen(lit->value) + 1;
                LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                LLVMValueRef args2[1] = { size };
                StrataAllocFn(b);
                LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args2, 1, "stra");
                LLVMBuildStore(b->m_builder, heap, lvalue.ptr);
                LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
                LLVMValueRef srcGep = rhs.value;
                LLVMValueRef dstGep = heap;
                for (size_t ci = 0; ci < copyLen; ci++)
                {
                    LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                    LLVMValueRef srcIdx[1] = { ciVal };
                    LLVMValueRef dstIdx[1] = { ciVal };
                    LLVMValueRef byteVal = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "srcas"), "bas");
                    LLVMBuildStore(b->m_builder, byteVal, LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "dstas"));
                }

                return rhs;
            }

            if (lvalue.typeDesc.isBox && !boxMove)
            {
                /* Assigning a plain T (or compound-assigning) into a box<T>
                   mutates its contents in place - not a move - so `x = 5;`
                   and `val -= amt;` both work even through a `ref box<T>`
                   param or a box global. lvalue.ptr is the
                   address of the box pointer slot (ref-adjusted already by
                   EmitLValue); load through it to get the box pointer, then
                   read/write the boxed value through that. */
                LLVMValueRef boxPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lvalue.ptr, "box");
                TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)lvalue.typeDesc.boxInner});
                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, innerTd.type, boxPtr, "boxval");
                Value result = Coerce(b, rhs, innerTd);
                bool flt = innerTd.isFloat;

                switch (n->op)
                {
                case AssignAdd:
                    result = ValueMake(flt
                        ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                        : LLVMBuildAdd(b->m_builder, cur, result.value, "add"), innerTd);
                    break;
                case AssignSub:
                    result = ValueMake(flt
                        ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                        : LLVMBuildSub(b->m_builder, cur, result.value, "sub"), innerTd);
                    break;
                case AssignMul:
                    result = ValueMake(flt
                        ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                        : LLVMBuildMul(b->m_builder, cur, result.value, "mul"), innerTd);
                    break;
                case AssignDiv:
                    result = ValueMake(flt
                        ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                        : (innerTd.isUnsigned
                            ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                            : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")), innerTd);
                    break;
                case AssignMod:
                    result = ValueMake(flt
                        ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                        : (innerTd.isUnsigned
                            ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                            : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")), innerTd);
                    break;
                default:
                    break;
                }

                LLVMBuildStore(b->m_builder, result.value, boxPtr);

                return result;
            }

            Value result = Coerce(b, rhs, lvalue.typeDesc);

            if (n->op != AssignSet)
            {
                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, lvalue.typeDesc.type, lvalue.ptr, "cur");
                bool flt = lvalue.typeDesc.isFloat;

                switch (n->op)
                {
                case AssignAdd:
                    result = ValueMake(flt
                        ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                        : LLVMBuildAdd(b->m_builder, cur, result.value, "add"), lvalue.typeDesc);
                    break;
                case AssignSub:
                    result = ValueMake(flt
                        ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                        : LLVMBuildSub(b->m_builder, cur, result.value, "sub"), lvalue.typeDesc);
                    break;
                case AssignMul:
                    result = ValueMake(flt
                        ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                        : LLVMBuildMul(b->m_builder, cur, result.value, "mul"), lvalue.typeDesc);
                    break;
                case AssignDiv:
                    result = ValueMake(flt
                        ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                        : (lvalue.typeDesc.isUnsigned
                            ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                            : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")), lvalue.typeDesc);
                    break;
                case AssignMod:
                    result = ValueMake(flt
                        ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                        : (lvalue.typeDesc.isUnsigned
                            ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                            : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")), lvalue.typeDesc);
                    break;
                default:
                    break;
                }
            }

            LLVMBuildStore(b->m_builder, result.value, lvalue.ptr);

            return result;
        }
    }

    if (b->m_diag)
    {
        DiagError(b->m_diag, n->base.range, "assignment to unsupported lvalue");
    }

    return rhs;
}

static LLVMValueRef ArgAddress(Builder* b, Node* arg)
{
    LValue lvalue = EmitLValue(b, arg);

    if (lvalue.valid)
    {
        return lvalue.ptr;
    }

    Value value = EmitExpr(b, arg);

    LLVMValueRef slot = EntryAlloca(b, value.typeDesc.type, "outarg");
    LLVMBuildStore(b->m_builder, value.value, slot);

    return slot;
}

/* Copies `count` elements from srcData to dstData. */
static void EmitArrayCopyLoop(Builder* b, LLVMValueRef dstData, LLVMValueRef srcData,
                              LLVMValueRef count, LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "acp.cond");
    LLVMBasicBlockRef body = NewBb(b, "acp.body");
    LLVMBasicBlockRef end = NewBb(b, "acp.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "acp.i");
    LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, count, "acplt");
    LLVMBuildCondBr(b->m_builder, cont, body, end);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = { i };
    LLVMValueRef srcEl = LLVMBuildGEP2(b->m_builder, elemTy, srcData, ep, 1, "s");
    LLVMValueRef dstEl = LLVMBuildGEP2(b->m_builder, elemTy, dstData, ep, 1, "d");
    LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, elemTy, srcEl, "e"), dstEl);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, end);
}

/* Zero-initializes elements [start, end) of data (nulls owning slots safely). */
static void EmitArrayZeroLoop(Builder* b, LLVMValueRef data, LLVMValueRef start,
                              LLVMValueRef end, LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "az.cond");
    LLVMBasicBlockRef body = NewBb(b, "az.body");
    LLVMBasicBlockRef endBb = NewBb(b, "az.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "az.i");
    LLVMBuildStore(b->m_builder, start, iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, end, "azlt");
    LLVMBuildCondBr(b->m_builder, cont, body, endBb);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = { i };
    LLVMValueRef el = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "z");
    LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), el);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, endBb);
}

/* Drops owning elements [start, end) of data before the buffer is freed. */
static void EmitArrayDropRange(Builder* b, LLVMValueRef data, LLVMValueRef start,
                               LLVMValueRef end, TypeDesc elemTd, LLVMTypeRef elemTy)
{
    LLVMBasicBlockRef cond = NewBb(b, "adr.cond");
    LLVMBasicBlockRef body = NewBb(b, "adr.body");
    LLVMBasicBlockRef endBb = NewBb(b, "adr.end");

    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "adr.i");
    LLVMBuildStore(b->m_builder, start, iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef cont = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, end, "adrlt");
    LLVMBuildCondBr(b->m_builder, cont, body, endBb);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef ep[1] = { i };
    LLVMValueRef el = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "drop");
    EmitDropOne(b, el, elemTd);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, endBb);
}

/* Inline array helpers: array_push / array_pop / array_resize.
   Each mutates the array through its lvalue and returns push->ulong, pop->T,
   resize->void. */
static Value EmitArrayBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    LValue arr = EmitLValue(b, arg0);

    TypeDesc elemTd = Resolve(b, &(TypeName){.name = (char*)arr.typeDesc.arrayInner});
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;
    TypeDesc ulongTd = TypeDescMake(I64Ty(b), false, true, false, NULL);

    LLVMValueRef dataPtrPtr = ArrayDataPtr(b, arr.ptr);
    LLVMValueRef lenPtr = ArrayLenPtr(b, arr.ptr);

    if (strcmp(n->callee, "array_pop") == 0)
    {
        LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I64Ty(b), lenPtr, "len");
        LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
        LLVMValueRef lm1 = LLVMBuildSub(b->m_builder, len, one, "lm1");
        LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
        LLVMValueRef idx[1] = { lm1 };
        LLVMValueRef elAddr = LLVMBuildGEP2(b->m_builder, elemTy, data, idx, 1, "pop");
        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, elemTy, elAddr, "popped");
        LLVMBuildStore(b->m_builder, lm1, lenPtr);

        return ValueMake(v, elemTd);
    }

    if (strcmp(n->callee, "array_push") == 0)
    {
        LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
        LLVMValueRef oldLen = LLVMBuildLoad2(b->m_builder, I64Ty(b), lenPtr, "len");
        LLVMValueRef newLen = LLVMBuildAdd(b->m_builder, oldLen, one, "nlen");
        LLVMValueRef oldData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");

        LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), newLen, "bytes");
        LLVMValueRef allocArgs[1] = { totalBytes };
        StrataAllocFn(b);
        LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "pushbuf");

        EmitArrayCopyLoop(b, newData, oldData, oldLen, elemTy);

        Node* valNode = (Node*)VecGet(&n->args, 1);
        LLVMValueRef slotIdx[1] = { oldLen };
        LLVMValueRef elAddr = LLVMBuildGEP2(b->m_builder, elemTy, newData, slotIdx, 1, "pushslot");
        Value v = EmitExpr(b, valNode);

        if (elemTd.isBox && !elemTd.boxInner && valNode->kind == NodeStrLiteral)
        {
            /* string literal -> fresh heap copy (don't store the global ptr). */
            StrLiteral* lit = AsNode(StrLiteral, valNode);
            size_t copyLen = strlen(lit->value) + 1;
            LLVMValueRef sz = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
            LLVMValueRef args[1] = { sz };
            StrataAllocFn(b);
            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "astr");
            LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
            for (size_t ci = 0; ci < copyLen; ci++)
            {
                LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                LLVMValueRef ba[1] = { ciVal };
                LLVMValueRef byte = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, v.value, ba, 1, "s"), "b");
                LLVMBuildStore(b->m_builder, byte, LLVMBuildGEP2(b->m_builder, i8Ty, heap, ba, 1, "d"));
            }
            LLVMBuildStore(b->m_builder, heap, elAddr);
        }
        else if (elemTd.isBox && v.typeDesc.isBox)
        {
            /* Move an owning source (string/box) into the slot, null it. */
            LLVMBuildStore(b->m_builder, v.value, elAddr);
            NullMovedSource(b, valNode);
        }
        else if (elemTd.isBox && elemTd.boxInner)
        {
            /* box<T> from a bare T literal/value: heap-box it inline. */
            TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)elemTd.boxInner});
            LLVMValueRef sz = SizeOfConst(b, innerTd.type);
            LLVMValueRef args[1] = { sz };
            StrataAllocFn(b);
            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "abox");
            LLVMBuildStore(b->m_builder, Coerce(b, v, innerTd).value, heap);
            LLVMBuildStore(b->m_builder, heap, elAddr);
        }
        else
        {
            LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elAddr);
        }

        LLVMValueRef freeArgs[1] = { oldData };
        StrataFreeFn(b);
        LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

        LLVMBuildStore(b->m_builder, newData, dataPtrPtr);
        LLVMBuildStore(b->m_builder, newLen, lenPtr);

        return ValueMake(newLen, ulongTd);
    }

    /* array_resize */
    LLVMValueRef oldLen = LLVMBuildLoad2(b->m_builder, I64Ty(b), lenPtr, "len");
    LLVMValueRef oldData = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
    LLVMValueRef newLen = AsI64Index(b, EmitExpr(b, (Node*)VecGet(&n->args, 1)));

    LLVMValueRef cmpLt = LLVMBuildICmp(b->m_builder, LLVMIntULT, oldLen, newLen, "lt");
    LLVMValueRef copyCount = LLVMBuildSelect(b->m_builder, cmpLt, oldLen, newLen, "cc");

    LLVMValueRef allocBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), newLen, "bytes");
    LLVMValueRef allocArgs[1] = { allocBytes };
    StrataAllocFn(b);
    LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "rszbuf");

    EmitArrayCopyLoop(b, newData, oldData, copyCount, elemTy);
    EmitArrayZeroLoop(b, newData, copyCount, newLen, elemTy);

    if (arr.typeDesc.arrayInner && IsOwningType(arr.typeDesc.arrayInner))
    {
        EmitArrayDropRange(b, oldData, newLen, oldLen, elemTd, elemTy);
    }

    LLVMValueRef freeArgs[1] = { oldData };
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

    LLVMBuildStore(b->m_builder, newData, dataPtrPtr);
    LLVMBuildStore(b->m_builder, newLen, lenPtr);

    return ValueMake(NULL, TypeDescMake(NULL, false, false, true, NULL));
}

static Value EmitCall(Builder* b, CallExpr* n)
{
    // Inline array helpers (array_push / array_pop / array_resize)?
    if (n->isPseudoCall
        && (strcmp(n->callee, "array_push") == 0
            || strcmp(n->callee, "array_pop") == 0
            || strcmp(n->callee, "array_resize") == 0))
    {
        return EmitArrayBuiltin(b, n);
    }

    // Is it a struct initializer call?
    if (StrMapGet(&b->m_structTypes, n->callee) != NULL)
    {
        TypeName tn = MakeTypeName(n->callee);
        TypeDesc typeDesc = Resolve(b, &tn);

        const StructType* st = TypeRegistryFind(&b->m_registry, n->callee);

        LLVMValueRef agg = LLVMConstNull(typeDesc.type);

        size_t nargs = n->args.count;

        for (size_t i = 0; i < nargs && st; i++)
        {
            if (i >= st->fields.count)
            {
                break;
            }

            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, i);
            TypeDesc fieldTd = Resolve(b, &fieldDecl->type);

            Value argValue = Coerce(b, EmitExpr(b, (Node*)VecGet(&n->args, i)), fieldTd);

            agg = LLVMBuildInsertValue(b->m_builder, agg, argValue.value, (unsigned)i, "ins");
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

    size_t nargs = n->args.count;
    LLVMValueRef* args = NULL;

    if (nargs > 0)
    {
        args = (LLVMValueRef*)arena_alloc(b->m_arena, nargs * sizeof(LLVMValueRef));
    }

    const FunctionDecl* fd = n->resolvedDecl;

    for (size_t k = 0; k < nargs; k++)
    {
        bool shouldPassByPtr = k < info->paramByPtrCount && info->paramByPtr[k];
        Node* argNode = (Node*)VecGet(&n->args, k);

        /* box<T> coerced to T: if the param is a plain struct (not box),
           and the arg is a box, the heap pointer IS the T* the param wants. */
        bool paramIsBoxType = fd && k < fd->params.count
            && IsOwningType(((ParamDecl*)VecGet(&fd->params, k))->type.name);

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

                argIsBox = sym && sym->typeDesc.isBox;
            }
            else if (argNode->kind == NodeCall)
            {
                FuncInfo* fi = (FuncInfo*)StrMapGet(&b->m_funcs, ((CallExpr*)argNode)->callee);
                argIsBox = fi && fi->returnType.isBox;
            }
        }

        if (shouldPassByPtr && paramIsBoxType && argNode->kind == NodeStrLiteral)
        {
            StrLiteral* lit = AsNode(StrLiteral, argNode);
            size_t copyLen = strlen(lit->value) + 1;
            
            LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
            LLVMValueRef args2[1] = { size };
            StrataAllocFn(b);

            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args2, 1, "argstr");

            Value litVal = EmitExpr(b, argNode);
            LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
            LLVMValueRef srcGep = litVal.value;
            LLVMValueRef dstGep = heap;

            for (size_t ci = 0; ci < copyLen; ci++)
            {
                LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                LLVMValueRef srcIdx[1] = { ciVal };
                LLVMValueRef dstIdx[1] = { ciVal };
                LLVMValueRef byteVal = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "asrc"), "ab");
                
                LLVMBuildStore(b->m_builder, byteVal, LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "adst"));
            }

            LLVMValueRef slot = EntryAlloca(b, b->m_ptrTy, "stslot");
            LLVMBuildStore(b->m_builder, heap, slot);

            args[k] = slot;
        }
        else if (shouldPassByPtr && !paramIsBoxType && argIsBox)
        {
            args[k] = EmitExpr(b, argNode).value;
        }
        else
        {
            /* A box arg passed to a by-value (non-indirect) param - e.g. a
               plain handle - must be dereferenced to its value, not passed
               as the box's own heap pointer. */
            args[k] = shouldPassByPtr ? ArgAddress(b, argNode) : DerefBoxValue(b, EmitExpr(b, argNode)).value;
        }
    }

    LLVMValueRef callee = info->function;

    if (b->m_jitMode)
    {
        LLVMValueRef slot = (LLVMValueRef)StrMapGet(&b->m_externSlots, n->callee);

        if (slot)
        {
            LLVMValueRef fnPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slot, "extfn");
            callee = fnPtr;
        }
    }

    LLVMValueRef call = LLVMBuildCall2(b->m_builder, info->type, callee, args, (unsigned)nargs, "call");

    return ValueMake(call, info->returnType);
}

static Value EmitArrayInit(Builder* b, ArrayInitExpr* n)
{
    TypeDesc elemTd = Resolve(b, &(TypeName){.name = n->elementType});

    /* The LLVM type of one element slot in the backing buffer. Arrays and
       boxes/strings both reduce to a pointer; structs/scalars use their
       own type. */
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

    size_t count = n->elements.count;
    LLVMValueRef dataPtr = LLVMConstNull(b->m_ptrTy);

    if (count > 0)
    {
        LLVMValueRef elemSize = SizeOfConst(b, elemTy);
        LLVMValueRef countConst = LLVMConstInt(I64Ty(b), (unsigned long long)count, 0);
        LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, elemSize, countConst, "bytes");
        LLVMValueRef allocArgs[1] = { totalBytes };
        StrataAllocFn(b);
        dataPtr = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "arrbuf");

        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);

        for (size_t i = 0; i < count; i++)
        {
            Node* eNode = (Node*)VecGet(&n->elements, i);
            Value v = EmitExpr(b, eNode);

            LLVMValueRef idxArg[1] = { LLVMConstInt(I64Ty(b), (unsigned long long)i, 0) };
            LLVMValueRef elemAddr = LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, idxArg, 1, "ael");

            if (elemTd.isArray)
            {
                /* Nested array literal element: recurse into the inline slot. */
                if (eNode->kind == NodeArrayInit)
                {
                    LLVMValueRef child = EmitArrayInit(b, AsNode(ArrayInitExpr, eNode)).value;
                    LLVMBuildStore(b->m_builder, child, elemAddr);
                }
                else
                {
                    LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elemAddr);
                }
            }
            else if (elemTd.isBox && elemTd.boxInner)
            {
                /* box<T> element. */
                if (v.typeDesc.isBox)
                {
                    /* Move from a box source: take its pointer, null the source. */
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                    NullMovedSource(b, eNode);
                }
                else
                {
                    /* Bare T literal/value: heap-box it inline. */
                    TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)elemTd.boxInner});
                    LLVMValueRef sz = SizeOfConst(b, innerTd.type);
                    LLVMValueRef args[1] = { sz };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "abox");
                    LLVMBuildStore(b->m_builder, Coerce(b, v, innerTd).value, heap);
                    LLVMBuildStore(b->m_builder, heap, elemAddr);
                }
            }
            else if (elemTd.isBox)
            {
                /* string element. */
                if (eNode->kind == NodeStrLiteral)
                {
                    StrLiteral* lit = AsNode(StrLiteral, eNode);
                    size_t copyLen = strlen(lit->value) + 1;
                    LLVMValueRef sz = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                    LLVMValueRef args[1] = { sz };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "astr");
                    for (size_t ci = 0; ci < copyLen; ci++)
                    {
                        LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                        LLVMValueRef ba[1] = { ciVal };
                        LLVMValueRef byte = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, v.value, ba, 1, "s"), "b");
                        LLVMBuildStore(b->m_builder, byte, LLVMBuildGEP2(b->m_builder, i8Ty, heap, ba, 1, "d"));
                    }
                    LLVMBuildStore(b->m_builder, heap, elemAddr);
                }
                else if (v.typeDesc.isBox)
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                    NullMovedSource(b, eNode);
                }
                else
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                }
            }
            else
            {
                LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elemAddr);
            }
        }
    }

    LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
    arr = LLVMBuildInsertValue(b->m_builder, arr, dataPtr, 0, "arri");
    arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I64Ty(b), (unsigned long long)count, 0), 1, "arrl");

    TypeDesc td = {0};
    td.type = ArrayStructType(b);
    td.isArray = true;
    td.arrayInner = arena_strdup(b->m_arena, n->elementType);

    return ValueMake(arr, td);
}

static Value EmitStructInit(Builder* b, StructInitExpr* n)
{
    TypeName tn = MakeTypeName(n->typeName);
    TypeDesc typeDesc = Resolve(b, &tn);

    const StructType* st = TypeRegistryFind(&b->m_registry, n->typeName);

    LLVMValueRef agg = LLVMConstNull(typeDesc.type);

    size_t positionalIndex = 0;

    for (size_t i = 0; i < n->fields.count; i++)
    {
        StructInitField* field = (StructInitField*)VecGet(&n->fields, i);

        size_t idx = 0;

        if (!field->name || field->name[0] == '\0')
        {
            idx = positionalIndex++;
        }
        else
        {
            int named = TypeRegistryFieldIndex(&b->m_registry, n->typeName, field->name);

            if (named < 0)
            {
                continue;
            }

            idx = (size_t)named;
        }

        if (!st || idx >= st->fields.count)
        {
            continue;
        }

        FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, idx);
        TypeDesc fieldTd = Resolve(b, &fieldDecl->type);

        Value rawField = EmitExpr(b, field->value);
        Value fieldValue;

        if (fieldTd.isBox && !rawField.typeDesc.isBox)
        {
            /* box<T> field initialized from a bare T value/literal - heap-box
               it (same as a top-level `box<T> x = T{...};` local), since the
               field is really a T* in the aggregate's LLVM layout. Inserting
               the raw T value there instead would insert a value of the
               wrong type into that slot - reinterpreted as a pointer, it
               corrupts memory the moment anything dereferences the field. */
            TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)fieldTd.boxInner});
            LLVMValueRef size = SizeOfConst(b, innerTd.type);
            LLVMValueRef args[1] = { size };
            StrataAllocFn(b);
            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "fieldbox");
            LLVMBuildStore(b->m_builder, rawField.value, heap);
            fieldValue = ValueMake(heap, fieldTd);
        }
        else
        {
            fieldValue = Coerce(b, rawField, fieldTd);
        }

        agg = LLVMBuildInsertValue(b->m_builder, agg, fieldValue.value, (unsigned)idx, "ins");
    }

    return ValueMake(agg, typeDesc);
}

static Value EmitExpr(Builder* b, Node* n)
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

        if (literal->value > 0xFFFFFFFFULL)
        {
            TypeDesc typeDesc = TypeDescMake(I64Ty(b), false, literal->isUnsigned, false, NULL);

            return ValueMake(LLVMConstInt(I64Ty(b), literal->value, 1), typeDesc);
        }

        TypeDesc typeDesc = TypeDescMake(I32Ty(b), false, literal->isUnsigned, false, NULL);

        return ValueMake(LLVMConstInt(I32Ty(b), literal->value, 1), typeDesc);
    }

    case NodeFloatLiteral:
    {
        FloatLiteral* literal = (FloatLiteral*)n;

        LLVMTypeRef fty = LLVMFloatTypeInContext(b->m_ctx);
        TypeDesc typeDesc = TypeDescMake(fty, true, false, false, NULL);

        return ValueMake(LLVMConstReal(fty, literal->value), typeDesc);
    }

    case NodeBoolLiteral:
    {
        BoolLiteral* literal = (BoolLiteral*)n;

        TypeDesc typeDesc = TypeDescMake(I1Ty(b), false, false, false, NULL);

        return ValueMake(LLVMConstInt(I1Ty(b), (unsigned long long)literal->value, 0), typeDesc);
    }

    case NodeStrLiteral:
    {
        StrLiteral* literal = (StrLiteral*)n;

        size_t len = strlen(literal->value);
        LLVMValueRef strConst = LLVMConstStringInContext(b->m_ctx, literal->value, (unsigned)len, 0);
        LLVMTypeRef strType = LLVMTypeOf(strConst);

        char* gName = arena_format(b->m_arena, ".str.%d", b->m_strLitCount++);
        LLVMValueRef global = LLVMAddGlobal(b->m_mod, strType, gName);
        LLVMSetInitializer(global, strConst);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(global, 1);
        LLVMSetGlobalConstant(global, 1);

        LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
        LLVMValueRef idx[2] = { zero, zero };
        LLVMValueRef gep = LLVMConstGEP2(strType, global, idx, 2);

        TypeDesc td = Resolve(b, &(TypeName){.name = "string"});

        return ValueMake(gep, td);
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

            return ValueMake(NULL, TypeDescMake(NULL, false, false, true, NULL));
        }

        LLVMValueRef valPtr = lv.ptr;
        TypeDesc valTd = lv.typeDesc;

        if (lv.typeDesc.isBox)
        {
            /* lv.ptr is the address of the box pointer slot (ref-adjusted
               already by EmitLValue); load through it to get the box
               pointer, then read/write the boxed value through that -
               same pattern as the box branch in EmitAssign. */
            valPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "box");
            valTd = Resolve(b, &(TypeName){.name = (char*)lv.typeDesc.boxInner});
        }

        LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, valTd.type, valPtr, "inc");
        LLVMValueRef one = valTd.isFloat
            ? LLVMConstReal(valTd.type, 1.0)
            : LLVMConstInt(valTd.type, 1, 0);

        LLVMValueRef newVal = inc->isDec
            ? (valTd.isFloat
                ? LLVMBuildFSub(b->m_builder, cur, one, "dec")
                : LLVMBuildSub(b->m_builder, cur, one, "dec"))
            : (valTd.isFloat
                ? LLVMBuildFAdd(b->m_builder, cur, one, "inc")
                : LLVMBuildAdd(b->m_builder, cur, one, "inc"));

        LLVMBuildStore(b->m_builder, newVal, valPtr);

        return ValueMake(inc->isPrefix ? newVal : cur, valTd);
    }

    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;
        Value operand = EmitExpr(b, cast->operand);
        TypeDesc target = Resolve(b, &cast->type);

        if (operand.typeDesc.isBox && target.isBox)
        {
            /* Retag with the destination type; Coerce would keep the source's. */
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

static void EmitStmt(Builder* b, Node* n)
{
    if (!n)
    {
        return;
    }

    switch (n->kind)
    {
    case NodeReturn:
    {
        ReturnStmt* r = (ReturnStmt*)n;
        Value v = {0};

        if (r->value)
        {
            Value raw = EmitExpr(b, r->value);

            if (b->m_curRet.isBox && !raw.typeDesc.isBox)
            {
                /* Returns box<T> but expr is a plain T: box it, like a local. */
                TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)b->m_curRet.boxInner});
                LLVMValueRef size = SizeOfConst(b, innerTd.type);
                LLVMValueRef args[1] = { size };
                StrataAllocFn(b);
                LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "heap");
                LLVMBuildStore(b->m_builder, Coerce(b, raw, innerTd).value, heap);
                v = ValueMake(heap, b->m_curRet);
            }
            else
            {
                v = UnboxIfBox(b, Coerce(b, raw, b->m_curRet), b->m_curRet);

                if (v.typeDesc.isBox && !v.typeDesc.boxInner && r->value->kind == NodeStrLiteral)
                {
                    StrLiteral* lit = AsNode(StrLiteral, r->value);
                    size_t copyLen = strlen(lit->value) + 1;
                    LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                    LLVMValueRef args2[1] = { size };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args2, 1, "strret");
                    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
                    LLVMValueRef srcGep = v.value;
                    LLVMValueRef dstGep = heap;
                    for (size_t ci = 0; ci < copyLen; ci++)
                    {
                        LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                        LLVMValueRef srcIdx[1] = { ciVal };
                        LLVMValueRef dstIdx[1] = { ciVal };
                        LLVMValueRef byteVal = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "srcr"), "br");
                        LLVMBuildStore(b->m_builder, byteVal, LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "dstr"));
                    }
                    v = ValueMake(heap, b->m_curRet);
                }

                Node* movedReturnNode = (v.typeDesc.isBox || v.typeDesc.isArray)
                    ? MovableBoxSourceNode(r->value)
                    : NULL;

                if (movedReturnNode)
                {
                    LValue src = EmitLValue(b, movedReturnNode);

                    if (src.valid)
                    {
                        if (src.typeDesc.isArray)
                        {
                            LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
                        }
                        else
                        {
                            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
                        }
                    }
                }
            }
        }

        EmitDrops(b, 0);

        if (r->value)
        {
            LLVMBuildRet(b->m_builder, v.value);
        }
        else
        {
            LLVMBuildRetVoid(b->m_builder);
        }

        b->m_terminated = true;

        return;
    }

    case NodeExprStmt:
    {
        ExprStmt* e = (ExprStmt*)n;

        if (e->expr)
        {
            (void)EmitExpr(b, e->expr);
        }

        return;
    }

    case NodeVarDecl:
    {
        VarDeclStmt* varDecl = (VarDeclStmt*)n;
        TypeDesc typeDesc = Resolve(b, &varDecl->type);

        if (typeDesc.isArray)
        {
            LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "arr");

            if (varDecl->init)
            {
                if (varDecl->init->kind == NodeArrayInit)
                {
                    LLVMValueRef arr = EmitArrayInit(b, AsNode(ArrayInitExpr, varDecl->init)).value;
                    LLVMBuildStore(b->m_builder, arr, slot);
                }
                else
                {
                    /* Move from another array binding: copy the {ptr,len}
                       struct and zero the source (whole slot) to avoid a
                       double free. */
                    Node* movedNode = MovableBoxSourceNode(varDecl->init);
                    LValue src = movedNode ? EmitLValue(b, movedNode) : (LValue){0};

                    Value value = EmitExpr(b, varDecl->init);
                    LLVMBuildStore(b->m_builder, Coerce(b, value, typeDesc).value, slot);

                    if (src.valid && src.typeDesc.isArray)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
                    }
                }
            }
            else
            {
                LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), slot);
            }

            Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
            sym->value = slot;
            sym->typeDesc = typeDesc;

            StrMapPut(&b->m_symbols, varDecl->name, sym);

            OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
            ol->slot = slot;
            ol->td = typeDesc;
            VecPush(&b->m_owningLocals, ol);

            return;
        }

        if (typeDesc.isBox)
        {
            LLVMValueRef slot = EntryAlloca(b, b->m_ptrTy, "box");

            if (varDecl->init)
            {
                Value value = EmitExpr(b, varDecl->init);

                bool isStrLiteral = varDecl->init->kind == NodeStrLiteral;

                if (value.typeDesc.isBox && !isStrLiteral)
                {
                    LLVMBuildStore(b->m_builder, value.value, slot);

                    Node* movedDeclNode = MovableBoxSourceNode(varDecl->init);

                    if (movedDeclNode)
                    {
                        LValue src = EmitLValue(b, movedDeclNode);

                        if (src.valid)
                        {
                            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
                        }
                    }
                }
                else if (isStrLiteral)
                {
                    StrLiteral* lit = AsNode(StrLiteral, varDecl->init);
                    size_t copyLen = strlen(lit->value) + 1;
                    LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                    LLVMValueRef args[1] = { size };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "str");
                    LLVMBuildStore(b->m_builder, heap, slot);

                    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
                    LLVMValueRef srcGep = value.value;
                    LLVMValueRef dstGep = heap;
                    LLVMValueRef idx = LLVMConstInt(I64Ty(b), 0, 0);
                    for (size_t ci = 0; ci < copyLen; ci++)
                    {
                        LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                        LLVMValueRef srcIdx[1] = { ciVal };
                        LLVMValueRef dstIdx[1] = { ciVal };
                        LLVMValueRef byteVal = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "src"), "b");
                        LLVMBuildStore(b->m_builder, byteVal, LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "dst"));
                    }
                }
                else if (typeDesc.boxInner)
                {
                    TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)typeDesc.boxInner});
                    LLVMValueRef size = SizeOfConst(b, innerTd.type);
                    LLVMValueRef args[1] = { size };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "heap");
                    LLVMBuildStore(b->m_builder, heap, slot);
                    LLVMBuildStore(b->m_builder, value.value, heap);
                }
            }
            else
            {
                LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), slot);
            }

            Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
            sym->value = slot;
            sym->typeDesc = typeDesc;

            StrMapPut(&b->m_symbols, varDecl->name, sym);

            OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
            ol->slot = slot;
            ol->td = typeDesc;
            VecPush(&b->m_owningLocals, ol);

            return;
        }

        LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "v");

        if (varDecl->init)
        {
            Value value = UnboxIfBox(b, Coerce(b, EmitExpr(b, varDecl->init), typeDesc), typeDesc);
            LLVMBuildStore(b->m_builder, value.value, slot);
        }
        else
        {
            LLVMBuildStore(b->m_builder, ZeroOf(typeDesc), slot);
        }

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
        sym->value = slot;
        sym->typeDesc = typeDesc;

        StrMapPut(&b->m_symbols, varDecl->name, sym);

        return;
    }

    case NodeBlock:
    {
        Block* blk = (Block*)n;
        size_t mark = b->m_owningLocals.count;

        for (size_t i = 0; i < blk->statements.count; i++)
        {
            EmitStmt(b, (Node*)VecGet(&blk->statements, i));
        }

        if (!b->m_terminated)
        {
            EmitDrops(b, mark);
        }

        b->m_owningLocals.count = mark;

        return;
    }

    case NodeIf:
    {
        IfStmt* i = (IfStmt*)n;
        LLVMValueRef cond = ToI1(b, EmitExpr(b, i->condition));

        LLVMBasicBlockRef thenBB = NewBb(b, "if.then");
        LLVMBasicBlockRef endBB = NewBb(b, "if.end");
        LLVMBasicBlockRef elseBB = i->elseBranch ? NewBb(b, "if.else") : endBB;

        LLVMBuildCondBr(b->m_builder, cond, thenBB, elseBB);

        b->m_terminated = true;

        PositionAtEnd(b, thenBB);
        EmitStmt(b, i->thenBranch);
        Br(b, endBB);

        if (i->elseBranch)
        {
            PositionAtEnd(b, elseBB);
            EmitStmt(b, i->elseBranch);
            Br(b, endBB);
        }

        PositionAtEnd(b, endBB);

        return;
    }

    case NodeWhile:
    {
        WhileStmt* w = (WhileStmt*)n;

        LLVMBasicBlockRef condBB = NewBb(b, "while.cond");
        LLVMBasicBlockRef bodyBB = NewBb(b, "while.body");
        LLVMBasicBlockRef endBB = NewBb(b, "while.end");

        LLVMBuildBr(b->m_builder, condBB);

        b->m_terminated = true;

        PositionAtEnd(b, condBB);

        LLVMValueRef cond = ToI1(b, EmitExpr(b, w->condition));

        LLVMBuildCondBr(b->m_builder, cond, bodyBB, endBB);

        b->m_terminated = true;

        PositionAtEnd(b, bodyBB);

        Loop* loop = (Loop*)arena_alloc(b->m_arena, sizeof(Loop));
        loop->cont = condBB;
        loop->end = endBB;
        VecPush(&b->m_loops, loop);

        EmitStmt(b, w->body);

        VecPop(&b->m_loops);

        Br(b, condBB);

        PositionAtEnd(b, endBB);

        return;
    }

    case NodeFor:
    {
        ForStmt* fs = (ForStmt*)n;

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
            LLVMBuildCondBr(b->m_builder, ToI1(b, EmitExpr(b, fs->condition)), bodyBB, endBB);
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
        VecPush(&b->m_loops, loop);

        EmitStmt(b, fs->body);

        VecPop(&b->m_loops);

        Br(b, updBB);

        PositionAtEnd(b, updBB);

        if (fs->update)
        {
            (void)EmitExpr(b, fs->update);
        }

        Br(b, condBB);

        PositionAtEnd(b, endBB);

        return;
    }

    case NodeBreak:
    {
        if (b->m_loops.count > 0)
        {
            Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
            Br(b, loop->end);
        }

        return;
    }

    case NodeContinue:
    {
        if (b->m_loops.count > 0)
        {
            Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
            Br(b, loop->cont);
        }

        return;
    }

    default:
    {
        (void)EmitExpr(b, n);
        return;
    }
    }
}

static BuiltModule BuilderBuild(Builder* b, const Module* module, DiagnosticEngine* diag, bool jitMode)
{
    b->m_diag = diag;
    b->m_jitMode = jitMode;
    b->m_ctx = LLVMContextCreate();
    b->m_mod = LLVMModuleCreateWithNameInContext(module->name, b->m_ctx);
    b->m_builder = LLVMCreateBuilderInContext(b->m_ctx);
    b->m_ptrTy = LLVMPointerTypeInContext(b->m_ctx, 0);
    b->m_strLitCount = 0;

    TypeRegistryBuild(&b->m_registry, module);

    for (size_t i = 0; i < b->m_registry.count; i++)
    {
        StructType* st = &b->m_registry.types[i];

        if (st->opaque)
        {
            StrMapPut(&b->m_structTypes, st->name, (void*)b->m_ptrTy);
        }
        else
        {
            char* irName = arena_format(b->m_arena, "struct.%s", st->name);
            LLVMTypeRef ty = LLVMStructCreateNamed(b->m_ctx, irName);
            StrMapPut(&b->m_structTypes, st->name, (void*)ty);
        }
    }

    for (size_t i = 0; i < b->m_registry.count; i++)
    {
        StructType* st = &b->m_registry.types[i];

        if (st->opaque)
        {
            continue;
        }

        size_t fcount = st->fields.count;
        LLVMTypeRef* fields = NULL;

        if (fcount > 0)
        {
            fields = (LLVMTypeRef*)arena_alloc(b->m_arena, fcount * sizeof(LLVMTypeRef));
        }

        for (size_t j = 0; j < fcount; j++)
        {
            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, j);
            fields[j] = Resolve(b, &fieldDecl->type).type;
        }

        LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, st->name);
        LLVMStructSetBody(structTy, fields, (unsigned)fcount, 0);
    }

    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);
        DeclareFunction(b, f);
    }

    for (size_t i = 0; i < module->globals.count; i++)
    {
        GlobalDecl* gd = (GlobalDecl*)VecGet(&module->globals, i);
        TypeDesc typeDesc = Resolve(b, &gd->type);

        if (IsOwningType(gd->type.name))
        {
            if (b->m_diag)
            {
                DiagErrorFmt(b->m_diag, gd->base.range,
                             "global '%s' has box type, which is not yet supported by the LLVM backend "
                             "(use the C/Tcc JIT)", gd->name);
            }

            continue;
        }

        LLVMValueRef init = LLVMConstNull(typeDesc.type);

        if (gd->init)
        {
            if (gd->init->kind == NodeIntLiteral)
            {
                init = LLVMConstInt(typeDesc.type, ((IntLiteral*)gd->init)->value, 0);
            }
            else if (gd->init->kind == NodeFloatLiteral)
            {
                init = LLVMConstReal(typeDesc.type, ((FloatLiteral*)gd->init)->value);
            }
            else if (gd->init->kind == NodeBoolLiteral)
            {
                init = LLVMConstInt(typeDesc.type, ((BoolLiteral*)gd->init)->value ? 1 : 0, 0);
            }
            else if (gd->init->kind == NodeUnary && ((UnaryExpr*)gd->init)->op == UnNeg)
            {
                Node* operand = ((UnaryExpr*)gd->init)->operand;
                if (operand->kind == NodeIntLiteral)
                {
                    init = LLVMConstInt(typeDesc.type, -((IntLiteral*)operand)->value, 1);
                }
                else if (operand->kind == NodeFloatLiteral)
                {
                    init = LLVMConstReal(typeDesc.type, -((FloatLiteral*)operand)->value);
                }
            }
        }

        LLVMValueRef global = LLVMAddGlobal(b->m_mod, typeDesc.type, gd->name);
        LLVMSetInitializer(global, init);

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
        sym->value = global;
        sym->typeDesc = typeDesc;
        StrMapPut(&b->m_globals, gd->name, sym);
    }

    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);
        DefineFunction(b, f);
    }

    for (size_t i = 0; i < module->functions.count; i++)
    {
        FunctionDecl* f = (FunctionDecl*)VecGet(&module->functions, i);

        if (f->isExtern)
        {
            VecPush(&b->m_externNames, f->name);
        }
    }

    if (b->m_builder)
    {
        LLVMDisposeBuilder(b->m_builder);
        b->m_builder = NULL;
    }

    BuiltModule out;
    BuiltModuleInit(&out);

    out.ctx = b->m_ctx;
    out.mod = b->m_mod;
    out.externSymbols = b->m_externNames;

    b->m_ctx = NULL;
    b->m_mod = NULL;

    return out;
}

void BuiltModuleInit(BuiltModule* bm)
{
    bm->ctx = NULL;
    bm->mod = NULL;
    VecInit(&bm->externSymbols);
}

void BuiltModuleDispose(BuiltModule* bm)
{
    if (bm->externSymbols.items)
    {
        free(bm->externSymbols.items);
        bm->externSymbols.items = NULL;
        bm->externSymbols.count = 0;
        bm->externSymbols.cap = 0;
    }
}

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode)
{
    Builder b = {0};
    b.m_arena = arena;
    StrMapInit(&b.m_structTypes);
    StrMapInit(&b.m_funcs);
    StrMapInit(&b.m_symbols);
    StrMapInit(&b.m_globals);
    StrMapInit(&b.m_externSlots);
    VecInit(&b.m_externNames);
    VecInit(&b.m_loops);
    VecInit(&b.m_owningLocals);
    b.m_allocFn = NULL;
    b.m_allocFnType = NULL;
    b.m_freeFn = NULL;
    b.m_freeFnType = NULL;
    b.m_arrayType = NULL;

    TypeRegistryInit(&b.m_registry);

    BuiltModule module = BuilderBuild(&b, ast, diag, jitMode);
    StrMapFree(&b.m_structTypes);
    StrMapFree(&b.m_funcs);
    StrMapFree(&b.m_symbols);
    StrMapFree(&b.m_globals);
    StrMapFree(&b.m_externSlots);
    free(b.m_loops.items);
    free(b.m_owningLocals.items);
    TypeRegistryFree(&b.m_registry);

    return module;
}
