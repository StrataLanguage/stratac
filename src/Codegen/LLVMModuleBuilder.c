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
    LLVMValueRef m_allocFn;
    LLVMTypeRef m_allocFnType;
    LLVMValueRef m_freeFn;
    LLVMTypeRef m_freeFnType;
    Arena* m_arena;
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
        if (TypeRegistryIsOwningStruct(&b->m_registry, t->name) && b->m_diag)
        {
            DiagErrorFmt(b->m_diag, t->range,
                         "owning structs are not yet supported by the LLVM backend (use the C/Tcc JIT)");
        }

        return TypeDescMake(found, false, false, false, t->name);
    }

    if (IsBoxTypeName(t->name))
    {
        char inner[128];
        TypeDesc td = TypeDescMake(b->m_ptrTy, false, false, false, NULL);

        if (BoxInnerTypeName(t->name, inner, sizeof inner))
        {
            td.isBox = true;
            td.boxInner = arena_strdup(b->m_arena, inner);
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

static Value Coerce(Builder* b, Value value, TypeDesc target)
{
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

static void EmitDropOne(Builder* b, LLVMValueRef slot)
{
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
        EmitDropOne(b, (LLVMValueRef)VecGet(&b->m_owningLocals, i));
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

        bool byPtr = p->mod != ModNone || structVal;
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

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));

        if (p->mod != ModNone || structVal)
        {
            sym->value = LLVMGetParam(b->m_curFn, (unsigned)i);
            sym->typeDesc = typeDesc;
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

    return none;
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

static Value EmitUnary(Builder* b, UnaryExpr* n)
{
    Value e = EmitExpr(b, n->operand);

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

        Value l = EmitExpr(b, n->lhs);

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
        Value r = EmitExpr(b, n->rhs);

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

    Value l = EmitExpr(b, n->lhs);
    Value r = EmitExpr(b, n->rhs);

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

    if (n->target->kind == NodeIdent || n->target->kind == NodeMember)
    {
        LValue lvalue = EmitLValue(b, n->target);

        if (lvalue.valid)
        {
            if (lvalue.typeDesc.isBox && n->op == AssignSet)
            {
                /* Box move: free the old value, take the new pointer, null the source. */
                EmitDropOne(b, lvalue.ptr);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                if (n->value->kind == NodeIdent)
                {
                    LValue src = EmitLValue(b, n->value);

                    if (src.valid)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
                    }
                }

                return rhs;
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

static Value EmitCall(Builder* b, CallExpr* n)
{
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

    for (size_t k = 0; k < nargs; k++)
    {
        bool shouldPassByPtr = k < info->paramByPtrCount && info->paramByPtr[k];
        Node* argNode = (Node*)VecGet(&n->args, k);

        args[k] = shouldPassByPtr ? ArgAddress(b, argNode) : EmitExpr(b, argNode).value;
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

        Value fieldValue = Coerce(b, EmitExpr(b, field->value), fieldTd);

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

        LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, lv.typeDesc.type, lv.ptr, "inc");
        LLVMValueRef one = lv.typeDesc.isFloat
            ? LLVMConstReal(lv.typeDesc.type, 1.0)
            : LLVMConstInt(lv.typeDesc.type, 1, 0);

        LLVMValueRef newVal = inc->isDec
            ? (lv.typeDesc.isFloat
                ? LLVMBuildFSub(b->m_builder, cur, one, "dec")
                : LLVMBuildSub(b->m_builder, cur, one, "dec"))
            : (lv.typeDesc.isFloat
                ? LLVMBuildFAdd(b->m_builder, cur, one, "inc")
                : LLVMBuildAdd(b->m_builder, cur, one, "inc"));

        LLVMBuildStore(b->m_builder, newVal, lv.ptr);

        return ValueMake(inc->isPrefix ? newVal : cur, lv.typeDesc);
    }

    case NodeCast:
    {
        CastExpr* cast = (CastExpr*)n;
        Value operand = EmitExpr(b, cast->operand);
        TypeDesc target = Resolve(b, &cast->type);

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
            v = Coerce(b, EmitExpr(b, r->value), b->m_curRet);

            /* Returning a box moves it out: null the source so the drop below
               does not free it. */
            if (v.typeDesc.isBox && r->value->kind == NodeIdent)
            {
                LValue src = EmitLValue(b, r->value);

                if (src.valid)
                {
                    LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
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

        if (typeDesc.isBox)
        {
            LLVMValueRef slot = EntryAlloca(b, b->m_ptrTy, "box");

            if (varDecl->init)
            {
                Value value = EmitExpr(b, varDecl->init);

                if (value.typeDesc.isBox)
                {
                    /* Move from another box<T>: take its pointer, null the source. */
                    LLVMBuildStore(b->m_builder, value.value, slot);

                    LValue src = EmitLValue(b, varDecl->init);

                    if (src.valid)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
                    }
                }
                else
                {
                    /* Box a value of the inner type. */
                    TypeDesc innerTd = Resolve(b, &(TypeName){.name = (char*)typeDesc.boxInner});
                    LLVMValueRef size = SizeOfConst(b, innerTd.type);
                    LLVMValueRef args[1] = { size };
                    StrataAllocFn(b);
                    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "heap");
                    LLVMBuildStore(b->m_builder, heap, slot);
                    LLVMBuildStore(b->m_builder, value.value, heap);
                }
            }

            Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
            sym->value = slot;
            sym->typeDesc = typeDesc;

            StrMapPut(&b->m_symbols, varDecl->name, sym);
            VecPush(&b->m_owningLocals, slot);

            return;
        }

        LLVMValueRef slot = EntryAlloca(b, typeDesc.type, "v");

        if (varDecl->init)
        {
            Value value = Coerce(b, EmitExpr(b, varDecl->init), typeDesc);
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
