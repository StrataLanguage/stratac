#include "LLVMModuleBuilder.h"
#include "AST/AST.h"
#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"
#include "LLVMSimd.h"
#include "TypeRegistry.h"
#include "TypeUtil.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    LLVMValueRef function;
    LLVMTypeRef type;
    TypeDesc returnType;
    bool* paramByPtr;
    size_t paramByPtrCount;
    bool externStringReturn; /* extern fn returning string: ABI is char*, caller wraps to fat */
} FuncInfo;

/* A folded compile-time constant initializer value. Kept as a tagged C value
 * (not an LLVMValueRef) so conversions happen host-side; the shipped LLVM-C
 * build does not export the LLVMConst*Cast family. */
typedef enum
{
    CIK_INT,
    CIK_FLOAT,
    CIK_BOOL
} ConstInitKind;

typedef struct
{
    ConstInitKind kind;
    unsigned long long i; /* CIK_INT / CIK_BOOL (0 or 1) */
    double f;             /* CIK_FLOAT */
} ConstInitVal;

/* A manifest constant: a `const` scalar global with a compile-time constant
   initializer. No LLVM global is emitted — uses inline the value. */
typedef struct
{
    ConstInitVal civ;
    TypeDesc td;
    LLVMValueRef constant;
} ConstValueSlot;

typedef struct
{
    bool valid;
    LLVMValueRef ptr;
    TypeDesc typeDesc;
} LValue;

/* A slot dropped (freed) on scope exit; carries its type so the drop can
   dispatch box/string vs array. */
typedef struct
{
    LLVMValueRef slot;
    TypeDesc td;
    bool stackBuffer; /* array backing is caller's stack: drop elems, not buffer */
} OwnLocal;

typedef struct
{
    LLVMBasicBlockRef cont;
    LLVMBasicBlockRef end;
    size_t headerMark; /* owning-locals count before the loop statement (dropped on break) */
    size_t bodyMark;   /* owning-locals count before the loop body (dropped on continue) */
    size_t scopeDepth; /* number of active defer scopes when the loop began (defers exit on break/continue) */
} Loop;

/* A `defer` scope: holds the statements queued to run at the block's exit. */
typedef struct
{
    Vec defers; /* Node* (DeferStmt.stmt) in source order; run LIFO */
} BlockScope;

typedef enum TypeDescFlag
{
    TD_FLOAT = (1 << 0),
    TD_UNSIGNED = (1 << 1),
    TD_VOID = (1 << 2),
    TD_VECTOR = (1 << 3),
} TypeDescFlag;

static TypeDesc TypeDescMake(LLVMTypeRef type, TypeDescFlag flags, const char* structTypeName)
{
    TypeDesc td = {0};
    td.type = type;
    td.isFloat = (flags & TD_FLOAT);
    td.isUnsigned = (flags & TD_UNSIGNED);
    td.isVoid = (flags & TD_VOID);
    td.isSimdVector = (flags & TD_VECTOR);
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

static TypeName MakeTypeName(Builder* b, const char* name)
{
    return TypeNameParse(b->m_arena, name);
}

static TypeDesc Resolve(Builder* b, const TypeName* t);

/* Resolve from a canonical spelling — only for the few spots that genuinely
   start from a plain name (e.g. "string"). */
static TypeDesc ResolveByName(Builder* b, const char* name)
{
    TypeName tn = MakeTypeName(b, name);
    return Resolve(b, &tn);
}

static const TypeName* StringTypeName(Builder* b)
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
    if (strcmp(p, "oeq") == 0)
    {
        return LLVMRealOEQ;
    }
    if (strcmp(p, "ogt") == 0)
    {
        return LLVMRealOGT;
    }
    if (strcmp(p, "oge") == 0)
    {
        return LLVMRealOGE;
    }
    if (strcmp(p, "olt") == 0)
    {
        return LLVMRealOLT;
    }
    if (strcmp(p, "ole") == 0)
    {
        return LLVMRealOLE;
    }
    if (strcmp(p, "one") == 0)
    {
        return LLVMRealONE;
    }

    return LLVMRealOEQ;
}

static LLVMValueRef IcmpByName(LLVMBuilderRef builder, const char* pred, bool isUnsigned, LLVMValueRef l,
                               LLVMValueRef r)
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

/*
   Return types smaller than 32-bit integers are returned sign/zero-extended
   in a 32-bit register (the callee is responsible for the extension). The ARM64
   ORC/JIT backend leaves the upper bits in the old wider value, so a C caller
   reading the full register at high optimization levels get garbage.

   Widen the LLVM return type to 32-bits so the register always holds a properly extended value.
*/
static LLVMTypeRef WidenRetType(Builder* b, LLVMTypeRef t)
{
    if (t && LLVMGetTypeKind(t) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(t) < 32)
    {
        return I32Ty(b);
    }

    return t;
}

/* Extend a value of the semantic return type into the (widened) ABI return
   register: zero-extend unsigned / bool, sign-extend signed variants. */
static LLVMValueRef ExtendToRetWidth(Builder* b, LLVMValueRef value, TypeDesc semantic, LLVMTypeRef retLlvm)
{
    if (retLlvm == semantic.type)
    {
        return value;
    }

    if (semantic.isUnsigned || semantic.type == I1Ty(b))
    {
        return LLVMBuildZExt(b->m_builder, value, retLlvm, "ret.zext");
    }

    return LLVMBuildSExt(b->m_builder, value, retLlvm, "ret.sext");
}

/* The shared {ptr, u64} fat struct for every T[] (cached so all arrays share one type). */
static LLVMValueRef IdxConst(Builder* b, unsigned i); /* forward decl */

static LLVMTypeRef ArrayStructType(Builder* b)
{
    if (!b->m_arrayType)
    {
        LLVMTypeRef fields[2] = {b->m_ptrTy, I64Ty(b)};
        b->m_arrayType = LLVMStructTypeInContext(b->m_ctx, fields, 2, 0);
    }

    return b->m_arrayType;
}

/* GEP helpers for the two array-slot fields: 0 = data ptr, 1 = length. */
static LLVMValueRef ArrayDataPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, 0)};
    return LLVMBuildGEP2(b->m_builder, ArrayStructType(b), slot, idx, 2, "aptr");
}

static LLVMValueRef ArrayLenPtr(Builder* b, LLVMValueRef slot)
{
    LLVMValueRef idx[2] = {IdxConst(b, 0), IdxConst(b, 1)};
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
        /* The entry block may already be terminated (e.g. drop code emitted
           on a return before any alloca was created). Insert before the
           terminator so the alloca stays in the entry prologue. */
        LLVMValueRef term = LLVMGetBasicBlockTerminator(b->m_entryBlock);

        if (term)
        {
            LLVMPositionBuilderBefore(b->m_builder, term);
        }
        else
        {
            LLVMPositionBuilderAtEnd(b->m_builder, b->m_entryBlock);
        }
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

static LLVMValueRef StrataAllocFn(Builder* b);

/* Assembles a fat string value from a data pointer and a length. */
static LLVMValueRef MakeStringFatValue(Builder* b, LLVMValueRef dataPtr, LLVMValueRef lenVal)
{
    LLVMValueRef fat = LLVMGetUndef(ArrayStructType(b));
    fat = LLVMBuildInsertValue(b->m_builder, fat, dataPtr, 0, "sfat.p");
    return LLVMBuildInsertValue(b->m_builder, fat, lenVal, 1, "sfat.l");
}

static LLVMValueRef IdxConst(Builder* b, unsigned i)
{
    /* All GEP indices must share one integer width; use i64 to stay consistent
       with the runtime (i64) indices produced by AsI64Index. */
    return LLVMConstInt(I64Ty(b), i, 0);
}

/* The TypeDesc of a `string` value: fat {ptr, len}, like T[]. */
static TypeDesc StringFatDesc(Builder* b)
{
    TypeDesc td = {0};
    td.type = ArrayStructType(b);
    td.isArray = true;
    td.isString = true;
    return td;
}

/* Builds an OWNED fat string value from srcPtr[0..len): allocates len+1
   bytes, copies, and writes the NUL terminator at [len] — the invariant
   the extern-boundary pun (fat -> char*) relies on. */
static LLVMValueRef BuildOwnedStringFat(Builder* b, LLVMValueRef srcPtr, LLVMValueRef lenVal)
{
    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
    LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
    LLVMValueRef copyBytes = LLVMBuildAdd(b->m_builder, lenVal, one, "slen");
    LLVMValueRef allocArgs[1] = {copyBytes};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "sbuf");

    /* Copy loop: heap[i] = srcPtr[i] for i in [0, len). */
    LLVMBasicBlockRef cond = NewBb(b, "scpy.cond");
    LLVMBasicBlockRef body = NewBb(b, "scpy.body");
    LLVMBasicBlockRef done = NewBb(b, "scpy.done");
    LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "scpy.i");
    LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
    Br(b, cond);

    PositionAtEnd(b, cond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
    LLVMValueRef more = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, lenVal, "slt");
    LLVMBuildCondBr(b->m_builder, more, body, done);
    b->m_terminated = true;

    PositionAtEnd(b, body);
    LLVMValueRef srcByte
        = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcPtr, &i, 1, "sb"), "b");
    LLVMBuildStore(b->m_builder, srcByte, LLVMBuildGEP2(b->m_builder, i8Ty, heap, &i, 1, "db"));
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, one, "next");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, done);
    /* NUL terminator at [len]. */
    LLVMValueRef termIdx[1] = {lenVal};
    LLVMBuildStore(b->m_builder, LLVMConstNull(i8Ty), LLVMBuildGEP2(b->m_builder, i8Ty, heap, termIdx, 1, "sterm"));

    return MakeStringFatValue(b, heap, lenVal);
}

/* Cached static NUL byte: a valid empty C string for the extern pun. */
static LLVMValueRef EmptyNulString(Builder* b)
{
    if (!b->m_emptyNul)
    {
        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
        LLVMTypeRef arrTy = LLVMArrayType(i8Ty, 1);
        LLVMValueRef global = LLVMAddGlobal(b->m_mod, arrTy, ".emptystr");
        LLVMSetInitializer(global, LLVMConstNull(arrTy));
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(global, 1);
        LLVMSetGlobalConstant(global, 1);

        LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
        LLVMValueRef idx[2] = {zero, zero};
        b->m_emptyNul = LLVMConstGEP2(arrTy, global, idx, 2);
    }

    return b->m_emptyNul;
}

/* Extracts the char* the C ABI sees for a string value: the data pointer,
   with a valid empty NUL-terminated buffer substituted when the string is
   the canonical empty {null, 0} (passing NULL to C string APIs is UB). */
static LLVMValueRef BuildExternStringPtr(Builder* b, Value v)
{
    LLVMValueRef dataPtr = LLVMBuildExtractValue(b->m_builder, v.value, 0, "sptr");
    LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, dataPtr, LLVMConstNull(b->m_ptrTy), "snull");

    return LLVMBuildSelect(b->m_builder, isNull, EmptyNulString(b), dataPtr, "sextern");
}

/* LLVM member index for logical field `idx` of a struct, mapping through the
   computed layout (explicit fieldoffsets insert padding members). */
static unsigned PhysicalFieldIndex(const StructType* st, int idx)
{
    if (st && st->hasLayout && st->physicalIndex && idx >= 0 && (size_t)idx < st->fields.count)
    {
        return (unsigned)st->physicalIndex[idx];
    }

    return (unsigned)(idx < 0 ? 0 : idx);
}

static TypeDesc Resolve(Builder* b, const TypeName* t);
static Value ZeroInt(Builder* b);
static Value Coerce(Builder* b, Value value, TypeDesc target);
static LLVMValueRef ToI1(Builder* b, Value v);
static void EmitStmt(Builder* b, Node* n);
Value EmitExpr(Builder* b, Node* n);
static Value EmitIdent(Builder* b, IdentExpr* n);
static LValue EmitLValue(Builder* b, Node* n);
static void EmitDropOne(Builder* b, LLVMValueRef slot, TypeDesc td);
static void EmitDummyStore(Builder* b, LLVMValueRef slot, TypeDesc td, Vec* chain);
static LLVMValueRef StrataStrdupFn(Builder* b);
static Value EmitMember(Builder* b, MemberExpr* n);
static Value EmitIndex(Builder* b, IndexExpr* n);
static Value EmitArrayInit(Builder* b, ArrayInitExpr* n);
static Value EmitArrayFromNodes(Builder* b, const TypeName* elementType, const Vec* elements, bool stackBuffer,
                                bool borrow);
static LLVMValueRef AsI64Index(Builder* b, Value v);
static Value EmitUnary(Builder* b, UnaryExpr* n);
static Value EmitBinary(Builder* b, BinaryExpr* n);
static Value EmitAssign(Builder* b, AssignExpr* n);
static Value EmitCall(Builder* b, CallExpr* n);
static Value EmitStructInit(Builder* b, StructInitExpr* n);
static LLVMValueRef ArgAddress(Builder* b, Node* arg);
static void DeclareFunction(Builder* b, const FunctionDecl* f);
static void DefineFunction(Builder* b, const FunctionDecl* f);

// -- Impl properties (methods are rewritten to plain extern calls by sema)

static LLVMValueRef ZeroOf(TypeDesc typeDesc);
static TypeDesc Resolve(Builder* b, const TypeName* t);

typedef struct
{
    PropertyDecl* decl;
    const FunctionDecl* getter; // NULL for write-only
    const FunctionDecl* setter; // NULL for read-only
} ImplPropEntry;

static const FunctionDecl* FindModuleFunction(const Module* module, const char* name)
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
static const char* StaticExprTypeName(Builder* b, Node* n)
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

        if ((sym->typeDesc.isBox || sym->typeDesc.isOptional) && sym->typeDesc.boxInner
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
                    inner = field->type.isArray ? field->type.elem : NULL;
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
        const TypeName* inner = (field->type.isBox || field->type.isOptional) ? field->type.inner : &field->type;

        return (inner && TypeRegistryIsOpaque(&b->m_registry, inner->name)) ? inner->name : NULL;
    }
    default:
        return NULL;
    }
}

/* Builds the getter/setter extern call for `m` (valueNode NULL = read).
   `found` reports whether `m` is an impl property at all. */
static Value EmitImplProperty(Builder* b, MemberExpr* m, Node* valueNode, bool* found)
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

/* Owning-ness with alias resolution — codegen counterpart of sema's
   AliasIsOwning: a `struct Name = string;` binding moves and drops exactly
   like its underlying type. Plain leaves are non-owning; a leaf alias is
   owning when its fully resolved underlying type is. */
static bool BuilderIsOwningType(Builder* b, const TypeName* t)
{
    if (!t || !t->name || TypeNameIsOwning(t))
    {
        return TypeNameIsOwning(t);
    }

    const char* leaf = TypeRegistryResolveAlias(&b->m_registry, t->name);

    if (!leaf || strcmp(leaf, t->name) == 0)
    {
        return false;
    }

    if (strcmp(leaf, "string") == 0)
    {
        return true;
    }

    TypeName parsed = TypeNameParse(b->m_arena, leaf);

    return TypeNameIsOwning(&parsed);
}

static TypeDesc Resolve(Builder* b, const TypeName* t)
{
    if (!t)
    {
        return TypeDescMake(NULL, TD_VOID, NULL);
    }

    MappedType mapped = MapType(t);

    if (mapped.valid)
    {
        TypeDescFlag flags = 0;
        if (mapped.isFloat)
        {
            flags |= TD_FLOAT;
        }
        if (mapped.isUnsigned)
        {
            flags |= TD_UNSIGNED;
        }
        if (mapped.isVoid)
        {
            flags |= TD_VOID;
        }
        if (mapped.isSimdVector)
        {
            flags |= TD_VECTOR;

            return TypeDescMake(VectorLlvmType(b->m_ctx, &mapped), flags, NULL);
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

    /* T[N]: fixed-size inline [N x T] (C ABI, struct fields only). */
    if (t->isArray && t->length >= 0)
    {
        TypeDesc elemTd = Resolve(b, t->elem);

        TypeDesc td = {0};
        td.type = LLVMArrayType(elemTd.type, (unsigned)t->length);
        td.isFixedArray = true;
        td.fixedLength = t->length;
        td.arrayInner = t->elem;

        return td;
    }

    if (t->isArray)
    {
        /* T[]: a fat {ptr, u64} struct (data pointer + length). The slot is
            always addressable (alloca / by-ref), so Resolve yields the struct
            type with the element type tagged on the side. */
        TypeDesc td = {0};
        td.type = ArrayStructType(b);
        td.isArray = true;
        td.arrayInner = t->elem;

        return td;
    }

    /* T[]?: an optional dynamic array. It shares the fat {ptr, u64}
        representation with T[] (empty = the canonical {null, 0}); the
        optional flag only drives sema-level narrowing, so the unwrap
        T[]? -> T[] is representation-identity. */
    if (t->isOptional && t->inner && t->inner->isArray && t->inner->length < 0)
    {
        TypeDesc td = Resolve(b, t->inner);
        td.isOptional = true;
        return td;
    }

    /* `string?` / `AliasOfString?`: same fat representation as string
       itself; empty = the canonical {null, 0}. */
    if (t->isOptional && t->inner && strcmp(TypeRegistryResolveAlias(&b->m_registry, t->inner->name), "string") == 0)
    {
        TypeDesc td = StringFatDesc(b);
        td.isOptional = true;
        return td;
    }

    /* `string` / alias-of-string: a fat {ptr, len} pair, exactly like T[].
       Empty = {null, 0} (nothing allocated); every constructed buffer
       carries a NUL terminator at [len], which the extern-boundary pun
       (fat -> char*) relies on. */
    if (strcmp(TypeRegistryResolveAlias(&b->m_registry, t->name), "string") == 0)
    {
        return StringFatDesc(b);
    }

    if (TypeNameIsOwning(t))
    {
        TypeDesc td = TypeDescMake(b->m_ptrTy, 0, NULL);
        const TypeName* boxInner = TypeNameBoxInner(t);

        td.isBox = true;
        /* `T?` shares the box representation; the flag only selects the
           whole-slot rebind path for assignments (contents may not exist). */
        td.isOptional = t->isOptional;

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

static LLVMValueRef ZeroOf(TypeDesc typeDesc)
{
    return LLVMConstNull(typeDesc.type);
}

static Value ZeroInt(Builder* b)
{
    return ValueMake(LLVMConstNull(I32Ty(b)), TypeDescMake(I32Ty(b), 0, NULL));
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

    TypeDesc innerTd = Resolve(b, value.typeDesc.boxInner);

    /* ^T where T is opaque (an incomplete struct or handle): the box cell
       holds the T* ITSELF, so unwrapping to T is identity - dereferencing
       again would read whatever the pointer points at as a pointer. */
    if (innerTd.structTypeName && TypeRegistryIsOpaque(&b->m_registry, innerTd.structTypeName))
    {
        return ValueMake(value.value, innerTd);
    }

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
        r = value.typeDesc.isUnsigned ? LLVMBuildUIToFP(b->m_builder, value.value, target.type, "c")
                                      : LLVMBuildSIToFP(b->m_builder, value.value, target.type, "c");
    }
    else if (value.typeDesc.isFloat && !target.isFloat)
    {
        r = target.isUnsigned ? LLVMBuildFPToUI(b->m_builder, value.value, target.type, "c")
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

/* If value is ^T but the target context expects T, load the pointee. */
static Value UnboxIfBox(Builder* b, Value value, TypeDesc target)
{
    if (value.typeDesc.isBox && !target.isBox && value.typeDesc.boxInner)
    {
        return DerefBoxValue(b, value);
    }

    return value;
}

static LLVMValueRef StrataAllocFn(Builder* b)
{
    if (!b->m_allocFn)
    {
        LLVMTypeRef params[1] = {I64Ty(b)};
        b->m_allocFnType = LLVMFunctionType(b->m_ptrTy, params, 1, 0);
        b->m_allocFn = LLVMAddFunction(b->m_mod, "strata_alloc", b->m_allocFnType);
    }

    return b->m_allocFn;
}

static LLVMValueRef StrataFreeFn(Builder* b)
{
    if (!b->m_freeFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_freeFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_freeFn = LLVMAddFunction(b->m_mod, "strata_free", b->m_freeFnType);
    }

    return b->m_freeFn;
}

static LLVMValueRef StrataPanicFn(Builder* b)
{
    if (!b->m_panicFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_panicFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_panicFn = LLVMAddFunction(b->m_mod, "strata_panic", b->m_panicFnType);
    }

    return b->m_panicFn;
}

static LLVMValueRef StrataOobFn(Builder* b)
{
    if (!b->m_oobFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_oobFnType = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), params, 1, 0);
        b->m_oobFn = LLVMAddFunction(b->m_mod, "strata_oob", b->m_oobFnType);
    }

    return b->m_oobFn;
}

/* A private constant global holding `message`, decayed to a char* (GEP 0,0). */
static LLVMValueRef MsgGlobalPtr(Builder* b, const char* prefix, const char* message)
{
    size_t len = strlen(message);
    LLVMValueRef strConst = LLVMConstStringInContext(b->m_ctx, message, (unsigned)len, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);
    char* gName = arena_format(b->m_arena, "%s.%d", prefix, b->m_strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->m_mod, strType, gName);
    LLVMSetInitializer(g, strConst);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef zero = LLVMConstInt(I32Ty(b), 0, 0);
    LLVMValueRef idx[2] = {zero, zero};
    return LLVMConstGEP2(strType, g, idx, 2);
}

/* Emits a call to strata_panic(message) followed by an unreachable terminator.
   Shared by bounds checks, null-extern-call checks, and any other runtime
   abort the profile emits. The current block must be terminated. */
static void EmitPanic(Builder* b, const char* message)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".pmsg", message)};
    StrataPanicFn(b);
    LLVMBuildCall2(b->m_builder, b->m_panicFnType, b->m_panicFn, args, 1, "");
    LLVMBuildUnreachable(b->m_builder);
}

/* Reports a recoverable out-of-bounds access (LLVM JIT): notifies the host's
   panic handler via strata_oob but returns, so the caller continues with a
   dummy element. */
static void EmitOobReport(Builder* b)
{
    LLVMValueRef args[1] = {MsgGlobalPtr(b, ".omsg", "array index out of bounds")};
    StrataOobFn(b);
    LLVMBuildCall2(b->m_builder, b->m_oobFnType, b->m_oobFn, args, 1, "");
}

/* Emits a bounds check: if index >= length, call strata_panic with the given
   message and halt (unreachable). Does nothing if index and length are
   compile-time constants and index < length. Skipped entirely when the
   StrataProfile disables bounds checking. */
static void EmitBoundsCheck(Builder* b, LLVMValueRef index, LLVMValueRef length)
{
    if (!b->m_boundsCheck)
    {
        return;
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, index, length, "oob");
    LLVMBasicBlockRef oobBB = NewBb(b, "oob");
    LLVMBasicBlockRef okBB = NewBb(b, "ok");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);

    PositionAtEnd(b, oobBB);
    {
        EmitPanic(b, "array index out of bounds");
    }
    b->m_terminated = true;
    PositionAtEnd(b, okBB);
}

static LLVMValueRef SizeOfConst(Builder* b, LLVMTypeRef ty)
{
    LLVMValueRef idx[1] = {LLVMConstInt(I64Ty(b), 1, 0)};
    LLVMValueRef gep = LLVMConstGEP2(ty, LLVMConstNull(b->m_ptrTy), idx, 1);

    return LLVMConstPtrToInt(gep, I64Ty(b));
}

/* JIT out-of-bounds: with boundsCheck on, bad indexes don't panic - reads
   resolve to a dummy element and writes are absorbed. Non-owning elements
   share a zeroed global; owning elements get a fresh dummy in per-site entry
   scratch (dropping whatever a prior OOB event left there). */

static LLVMValueRef EntryAllocaZeroed(Builder* b, LLVMTypeRef type, const char* name)
{
    LLVMValueRef before = b->m_entryAllocaPt;
    LLVMValueRef slot = EntryAlloca(b, type, name);
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(b->m_builder);

    /* EntryAlloca inserted the slot just before `before` (or the entry
       terminator); positioning there puts the zeroing store right after it. */
    if (before)
    {
        LLVMPositionBuilderBefore(b->m_builder, before);
    }
    else
    {
        LLVMValueRef term = LLVMGetBasicBlockTerminator(b->m_entryBlock);

        if (term)
        {
            LLVMPositionBuilderBefore(b->m_builder, term);
        }
        else
        {
            LLVMPositionBuilderAtEnd(b->m_builder, b->m_entryBlock);
        }
    }

    LLVMBuildStore(b->m_builder, LLVMConstNull(type), slot);
    LLVMPositionBuilderAtEnd(b->m_builder, cur);
    return slot;
}

static LLVMValueRef SharedDummyAddr(Builder* b, TypeDesc td)
{
    char* gName = arena_format(b->m_arena, ".oob.zero.%d", b->m_strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->m_mod, td.type, gName);
    LLVMSetInitializer(g, LLVMConstNull(td.type));
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMValueRef idx[1] = {LLVMConstInt(I32Ty(b), 0, 0)};
    return LLVMConstGEP2(td.type, g, idx, 1);
}

static LLVMValueRef EmptyStrConstant(Builder* b)
{
    char nul[1] = {0};
    LLVMValueRef strConst = LLVMConstStringInContext(b->m_ctx, nul, 1, 0);
    LLVMTypeRef strType = LLVMTypeOf(strConst);
    char* gName = arena_format(b->m_arena, ".oob.empty.%d", b->m_strLitCount++);
    LLVMValueRef g = LLVMAddGlobal(b->m_mod, strType, gName);
    LLVMSetInitializer(g, strConst);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    LLVMSetGlobalConstant(g, 1);
    LLVMValueRef idx[2] = {LLVMConstInt(I32Ty(b), 0, 0), LLVMConstInt(I32Ty(b), 0, 0)};
    return LLVMConstGEP2(strType, g, idx, 2);
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
static bool ElemNeedsScratchDummy(Builder* b, TypeDesc td)
{
    if (td.isArray || td.isBox)
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

    if (td.isArray)
    {
        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), slot);
        return;
    }

    if (td.isBox && !td.boxInner)
    {
        StrataStrdupFn(b);
        LLVMValueRef args[1] = {EmptyStrConstant(b)};
        LLVMValueRef dup = LLVMBuildCall2(b->m_builder, b->m_strdupFnType, b->m_strdupFn, args, 1, "dstr");
        LLVMBuildStore(b->m_builder, dup, slot);
        return;
    }

    if (td.isBox && td.boxInner)
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

            LLVMValueRef allocArgs[1] = {SizeOfConst(b, innerTd.type)};
            StrataAllocFn(b);
            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "dbox");

            EmitDummyStoreStructFields(b, heap, st, structTy, chain, td.boxInner->name);
            LLVMBuildStore(b->m_builder, heap, slot);
            return;
        }

        /* Inner is a scalar, string, another box, or an array: allocate and
           initialize the single inner value. */
        LLVMValueRef allocArgs[1] = {SizeOfConst(b, innerTd.type)};
        StrataAllocFn(b);
        LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "dbox");

        if (innerTd.isArray || innerTd.isBox)
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

static void EmitDummyInit(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    Vec chain;
    VecInit(&chain);
    EmitDummyStore(b, slot, td, &chain);
    free(chain.items);
}

/* In-bounds element address; aliased rest arrays hold T* slots, so load the slot. */
static LLVMValueRef PlainElemPtr(Builder* b, LLVMValueRef dataPtr, LLVMValueRef idxVal, LLVMTypeRef elemTy,
                                 bool aliased)
{
    LLVMValueRef idxArgs[1] = {idxVal};

    if (aliased)
    {
        LLVMValueRef slotAddr = LLVMBuildGEP2(b->m_builder, b->m_ptrTy, dataPtr, idxArgs, 1, "ais");
        return LLVMBuildLoad2(b->m_builder, b->m_ptrTy, slotAddr, "alias");
    }

    return LLVMBuildGEP2(b->m_builder, elemTy, dataPtr, idxArgs, 1, "ai");
}

/* Resolves arr[idx] under the profile bounds check: boundsCheck off -> raw
   GEP; AOT -> panic branch; JIT -> no abort, report via strata_oob and
   continue with a dummy slot (reads yield a dummy, writes absorbed). Owning
   elements rebuild scratch per event; null-store re-resolution skips that. */
static LLVMValueRef EmitCheckedElemPtr(Builder* b, LLVMValueRef idxVal, LLVMValueRef lenVal, LLVMValueRef dataPtr,
                                       LLVMTypeRef elemTy, TypeDesc elemTd, bool aliased)
{
    if (!b->m_boundsCheck)
    {
        return PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    }

    if (!b->m_jitMode)
    {
        EmitBoundsCheck(b, idxVal, lenVal);
        return PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, idxVal, lenVal, "oob");

    LLVMValueRef scratch = NULL;

    if (ElemNeedsScratchDummy(b, elemTd))
    {
        scratch = EntryAllocaZeroed(b, elemTd.type ? elemTd.type : b->m_ptrTy, "oob.slot");
    }

    LLVMBasicBlockRef oobBB = NewBb(b, "oob.dummy");
    LLVMBasicBlockRef okBB = NewBb(b, "oob.ok");
    LLVMBasicBlockRef mergeBB = NewBb(b, "oob.elem");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);
    b->m_terminated = true;

    PositionAtEnd(b, oobBB);

    /* A bookkeeping re-resolution of the same expression (null-the-source
       after a move) is not a new access: the access itself already reported. */
    if (!b->m_nullStoreLValue)
    {
        EmitOobReport(b);
    }

    LLVMValueRef dummyAddr;

    if (scratch)
    {
        dummyAddr = scratch;

        if (!b->m_nullStoreLValue)
        {
            EmitDropOne(b, scratch, elemTd);
            EmitDummyInit(b, scratch, elemTd);
        }
    }
    else
    {
        dummyAddr = SharedDummyAddr(b, elemTd);
    }

    Br(b, mergeBB);
    LLVMBasicBlockRef oobEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, okBB);
    LLVMValueRef elemAddr = PlainElemPtr(b, dataPtr, idxVal, elemTy, aliased);
    Br(b, mergeBB);
    LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->m_builder, b->m_ptrTy, "el");
    LLVMValueRef incomingVals[2] = {dummyAddr, elemAddr};
    LLVMBasicBlockRef incomingBlocks[2] = {oobEnd, okEnd};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
    return phi;
}

/* Element address in a fixed inline [N x T]; the leading 0 walks into the array. */
static LLVMValueRef FixedElemPtr(Builder* b, LLVMValueRef arrPtr, LLVMValueRef idxVal, LLVMTypeRef arrTy)
{
    LLVMValueRef idxArgs[2] = {IdxConst(b, 0), idxVal};
    return LLVMBuildGEP2(b->m_builder, arrTy, arrPtr, idxArgs, 2, "ai");
}

/* Bounds-checked variant for fixed arrays. The length is the compile-time
   dimension; elements are never owning (enforced by sema), so the JIT dummy
   is a shared slot and never needs per-site scratch. */
static LLVMValueRef EmitCheckedFixedElemPtr(Builder* b, LLVMValueRef idxVal, long length, LLVMValueRef arrPtr,
                                            LLVMTypeRef arrTy, TypeDesc elemTd)
{
    LLVMValueRef lenVal = LLVMConstInt(I64Ty(b), (unsigned long long)(length < 0 ? 0 : length), 0);

    if (!b->m_boundsCheck)
    {
        return FixedElemPtr(b, arrPtr, idxVal, arrTy);
    }

    if (!b->m_jitMode)
    {
        EmitBoundsCheck(b, idxVal, lenVal);
        return FixedElemPtr(b, arrPtr, idxVal, arrTy);
    }

    LLVMValueRef oob = LLVMBuildICmp(b->m_builder, LLVMIntUGE, idxVal, lenVal, "oob");

    LLVMBasicBlockRef oobBB = NewBb(b, "oob.dummy");
    LLVMBasicBlockRef okBB = NewBb(b, "oob.ok");
    LLVMBasicBlockRef mergeBB = NewBb(b, "oob.elem");
    LLVMBuildCondBr(b->m_builder, oob, oobBB, okBB);
    b->m_terminated = true;

    PositionAtEnd(b, oobBB);

    if (!b->m_nullStoreLValue)
    {
        EmitOobReport(b);
    }

    LLVMValueRef dummyAddr = SharedDummyAddr(b, elemTd);

    Br(b, mergeBB);
    LLVMBasicBlockRef oobEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, okBB);
    LLVMValueRef elemAddr = FixedElemPtr(b, arrPtr, idxVal, arrTy);
    Br(b, mergeBB);
    LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(b->m_builder);

    PositionAtEnd(b, mergeBB);
    LLVMValueRef phi = LLVMBuildPhi(b->m_builder, b->m_ptrTy, "el");
    LLVMValueRef incomingVals[2] = {dummyAddr, elemAddr};
    LLVMBasicBlockRef incomingBlocks[2] = {oobEnd, okEnd};
    LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
    return phi;
}

/* Heap-copies a NUL-terminated string so the copy can be owned and freed. */
static LLVMValueRef HeapCopyString(Builder* b, LLVMValueRef srcPtr, size_t len)
{
    size_t copyLen = len + 1;
    LLVMValueRef sz = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
    LLVMValueRef args[1] = {sz};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "str");

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);

    for (size_t ci = 0; ci < copyLen; ci++)
    {
        LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
        LLVMValueRef ba[1] = {ciVal};
        LLVMValueRef byte
            = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcPtr, ba, 1, "s"), "b");
        LLVMBuildStore(b->m_builder, byte, LLVMBuildGEP2(b->m_builder, i8Ty, heap, ba, 1, "d"));
    }

    return heap;
}

/* Drops the owning fields of a struct at `structPtr`. */
static void EmitDropStructFields(Builder* b, LLVMValueRef structPtr, const char* structName);

/* Returns (creating if needed) a per-struct drop fn for `structName`'s owning
   fields, used instead of inlining at ^T drop sites. A self-referential/
   mutually recursive owning struct would otherwise recurse without bound at
   IR-gen time; a real call terminates via the callee's null check. */
static LLVMValueRef GetOrCreateStructDropFn(Builder* b, const char* structName);

/* Drops the owning value in `slot`, typed `td`: ^T/string free the pointer
   (and a boxed owning struct's fields first); T[] frees the backing buffer and
   drops owning elements via a loop. freeArrayBuffer=false (vararg rest) drops
   elements but not the caller's stack buffer. */
static void EmitDropOneInternal(Builder* b, LLVMValueRef slot, TypeDesc td, bool freeArrayBuffer)
{
    if (td.isArray)
    {
        LLVMValueRef dataPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ArrayDataPtr(b, slot), "adrop");

        if (td.arrayInner && BuilderIsOwningType(b, td.arrayInner))
        {
            LLVMValueRef lenVal = LLVMBuildLoad2(b->m_builder, I64Ty(b), ArrayLenPtr(b, slot), "adlen");
            TypeDesc elemTd = Resolve(b, td.arrayInner);
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
            LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), ArrayLenPtr(b, slot));
        }

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

    /* A box of an owning inner owns that inner too. An owning struct drops
       its fields recursively; an owning primitive (string) is a single heap
       pointer freed directly; a plain value (int) needs no inner drop. */
    if (td.boxInner)
    {
        if (TypeRegistryIsOwningStruct(&b->m_registry, td.boxInner->name))
        {
            LLVMValueRef dropFn = GetOrCreateStructDropFn(b, td.boxInner->name);
            LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), &b->m_ptrTy, 1, 0);
            LLVMValueRef dropArgs[1] = {ptr};
            LLVMBuildCall2(b->m_builder, dropFnTy, dropFn, dropArgs, 1, "");
        }
        else if (BuilderIsOwningType(b, td.boxInner))
        {
            LLVMValueRef innerPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, ptr, "bin");
            LLVMValueRef iargs[1] = {innerPtr};
            StrataFreeFn(b);
            LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, iargs, 1, "");
        }
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

static void EmitDropOne(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    EmitDropOneInternal(b, slot, td, true);
}

/* Drops owning ELEMENTS of a stack-backed T[] (vararg rest) without freeing the buffer. */
static void EmitDropOneStack(Builder* b, LLVMValueRef slot, TypeDesc td)
{
    EmitDropOneInternal(b, slot, td, false);
}

static void EmitDropStructFields(Builder* b, LLVMValueRef structPtr, const char* structName)
{
    const StructType* st = TypeRegistryFind(&b->m_registry, structName);

    if (!st)
    {
        return;
    }

    LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, structName);

    if (!structTy)
    {
        return;
    }

    for (size_t j = 0; j < st->fields.count; j++)
    {
        FieldDecl* f = (FieldDecl*)VecGet(&st->fields, j);
        bool fieldOwning = BuilderIsOwningType(b, &f->type);

        /* A plain owning struct held by value is normally rejected (must be
           boxed), but handle it defensively for completeness. */
        bool fieldOwningStruct = TypeRegistryIsOwningStruct(&b->m_registry, f->type.name);

        if (!fieldOwning && !fieldOwningStruct)
        {
            continue;
        }

        LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)j))};
        LLVMValueRef fieldAddr = LLVMBuildGEP2(b->m_builder, structTy, structPtr, idxs, 2, "fd");

        if (fieldOwning)
        {
            TypeDesc fieldTd = Resolve(b, &f->type);
            EmitDropOne(b, fieldAddr, fieldTd);
        }
        else
        {
            EmitDropStructFields(b, fieldAddr, f->type.name);
        }
    }
}

/* Builds a drop fn body: `if (!p) return;` then EmitDropStructFields(p). May
   run mid-emission of another function (a ^T drop can be reached anywhere),
   so the enclosing build context is saved and restored around it. */
static void EmitStructDropFnBody(Builder* b, LLVMValueRef fn, const char* structName)
{
    LLVMValueRef savedCurFn = b->m_curFn;
    LLVMBasicBlockRef savedEntryBlock = b->m_entryBlock;
    LLVMValueRef savedEntryAllocaPt = b->m_entryAllocaPt;
    bool savedTerminated = b->m_terminated;
    LLVMBasicBlockRef savedInsertBlock = LLVMGetInsertBlock(b->m_builder);

    b->m_curFn = fn;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, fn, "entry");
    PositionAtEnd(b, entry);
    b->m_entryBlock = entry;
    b->m_entryAllocaPt = NULL;

    LLVMValueRef p = LLVMGetParam(fn, 0);

    LLVMBasicBlockRef nullBB = NewBb(b, "sdrop.null");
    LLVMBasicBlockRef doBB = NewBb(b, "sdrop.do");

    LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, p, LLVMConstNull(b->m_ptrTy), "isnull");
    LLVMBuildCondBr(b->m_builder, isNull, nullBB, doBB);
    b->m_terminated = true;

    PositionAtEnd(b, nullBB);
    LLVMBuildRetVoid(b->m_builder);
    b->m_terminated = true;

    PositionAtEnd(b, doBB);

    EmitDropStructFields(b, p, structName);

    if (!b->m_terminated)
    {
        LLVMBuildRetVoid(b->m_builder);
        b->m_terminated = true;
    }

    b->m_curFn = savedCurFn;
    b->m_entryBlock = savedEntryBlock;
    b->m_entryAllocaPt = savedEntryAllocaPt;
    b->m_terminated = savedTerminated;

    if (savedInsertBlock)
    {
        LLVMPositionBuilderAtEnd(b->m_builder, savedInsertBlock);
    }
}

static LLVMValueRef GetOrCreateStructDropFn(Builder* b, const char* structName)
{
    LLVMValueRef existing = (LLVMValueRef)StrMapGet(&b->m_dropFns, structName);

    if (existing)
    {
        return existing;
    }

    LLVMTypeRef fnTy = LLVMFunctionType(LLVMVoidTypeInContext(b->m_ctx), &b->m_ptrTy, 1, 0);
    char* fnName = arena_format(b->m_arena, "__strata_drop_%s", structName);
    LLVMValueRef fn = LLVMAddFunction(b->m_mod, fnName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    /* Register before emitting the body: a self-referential or mutually
       recursive struct's own drop body will look this function back up
       (cache hit) instead of triggering another EmitStructDropFnBody call. */
    StrMapPut(&b->m_dropFns, structName, (void*)fn);

    EmitStructDropFnBody(b, fn, structName);

    return fn;
}

static void EmitDrops(Builder* b, size_t fromIndex)
{
    for (size_t i = fromIndex; i < b->m_owningLocals.count; i++)
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

/* A `defer` scope: deferred statements queued for the enclosing block's exit. */

static void PushScope(Builder* b)
{
    BlockScope* bs = (BlockScope*)arena_alloc(b->m_arena, sizeof(BlockScope));
    VecInit(&bs->defers);
    VecPush(&b->m_scopes, bs);
}

static void PopScope(Builder* b)
{
    VecPop(&b->m_scopes);
}

static BlockScope* TopScope(Builder* b)
{
    if (b->m_scopes.count == 0)
    {
        return NULL;
    }

    return (BlockScope*)VecGet(&b->m_scopes, b->m_scopes.count - 1);
}

/* Run one scope's deferred statements, LIFO (last `defer` first), Zig-style. */
static void RunDefers(Builder* b, BlockScope* scope)
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
static void RunDefersFrom(Builder* b, size_t fromIndex)
{
    for (size_t s = b->m_scopes.count; s-- > fromIndex;)
    {
        RunDefers(b, (BlockScope*)VecGet(&b->m_scopes, s));
    }
}

static void DeclareFunction(Builder* b, const FunctionDecl* f)
{
    FuncInfo* info = (FuncInfo*)arena_alloc(b->m_arena, sizeof(FuncInfo));

    bool hasReturnParam = FunctionHasReturnParam(f);

    info->returnType = Resolve(b, &f->returnType);
    info->returnType.type = WidenRetType(b, info->returnType.type);

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

        bool structVal = TypeRegistryIsUserType(&b->m_registry, p->type.name)
                         && !TypeRegistryIsOpaque(&b->m_registry, p->type.name);

        bool byPtr = p->mod != ModNone || structVal || BuilderIsOwningType(b, &p->type);

        /* Extern string params pun to char*: a plain string (or alias) passes
           its data pointer (NUL-terminated); an optional `string?` passes the
           raw pointer (NULL = empty). The host reads without taking
           ownership. */
        bool optionalString = p->type.isOptional && p->type.inner
                              && strcmp(TypeRegistryResolveAlias(&b->m_registry, p->type.inner->name), "string") == 0;
        bool externStringParam
            = f->isExtern
              && (strcmp(TypeRegistryResolveAlias(&b->m_registry, p->type.name), "string") == 0 || optionalString);

        if (byPtr && externStringParam)
        {
            byPtr = false;
        }

        /* For extern functions, ^T / T? params cross as the pointer ITSELF
           by value (one `ptr`), not as a pointer to the caller's slot -
           matching the ABI hosts see for `T*` / `T?` parameters. */
        if (byPtr && f->isExtern && (p->type.isBox || p->type.isOptional))
        {
            byPtr = false;
        }

        info->paramByPtr[i] = byPtr;

        /* Extern string params pun to char* (the fat's first member). */
        params[i] = byPtr ? b->m_ptrTy : (externStringParam ? b->m_ptrTy : Resolve(b, &p->type).type);
    }

    /* An extern function returning string: the C ABI is char* (one ptr).
       The caller wraps it back into a fat via an inline strlen. Internal
       (defined) functions return the fat struct itself. A `return` param
       (out pointer) never uses this: its type comes back through the
       pointer, so the ABI return stays void. */
    info->externStringReturn = f->isExtern && !hasReturnParam
                               && strcmp(TypeRegistryResolveAlias(&b->m_registry, f->returnType.name), "string") == 0;

    /* A `return` param means the C function is void ret + out-pointer:
       the Strata-level return type never reaches the return register. */
    LLVMTypeRef abiRetType = hasReturnParam ? LLVMVoidTypeInContext(b->m_ctx)
                                            : (info->externStringReturn ? b->m_ptrTy : info->returnType.type);

    info->type = LLVMFunctionType(abiRetType, params, (unsigned)pcount, f->isCVararg ? 1 : 0);

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
    b->m_curRetAbi = WidenRetType(b, b->m_curRet.type);
    b->m_loops.count = 0;
    b->m_owningLocals.count = 0;
    b->m_scopes.count = 0;

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

        bool structVal = TypeRegistryIsUserType(&b->m_registry, p->type.name)
                         && !TypeRegistryIsOpaque(&b->m_registry, p->type.name);
        bool boxParam = BuilderIsOwningType(b, &p->type);

        Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));

        if (p->mod != ModNone || structVal || boxParam)
        {
            sym->value = LLVMGetParam(b->m_curFn, (unsigned)i);
            sym->typeDesc = typeDesc;

            /* A `ref T... rest` with non-owning elements collects pointers to
               the source arguments; element access derefs the slot. */
            if (p->isVarargRest && p->mod == ModRef)
            {
                const TypeName* elem = TypeNameArrayElem(&p->type);

                if (elem && !BuilderIsOwningType(b, elem))
                {
                    sym->typeDesc.aliasedArray = true;
                }
            }

            /* An owned (non-ref) box/array parameter is consumed: freed at return.
               A vararg rest array is stack-backed: its owning ELEMENTS are
               dropped, but the caller's stack buffer is not freed. */
            if (boxParam && p->mod == ModNone)
            {
                OwnLocal* ol = (OwnLocal*)arena_alloc(b->m_arena, sizeof(OwnLocal));
                ol->slot = sym->value;
                ol->td = typeDesc;
                ol->stackBuffer = p->isVarargRest;
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

    PushScope(b);

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
        RunDefers(b, TopScope(b));
    }

    PopScope(b);

    if (!b->m_terminated)
    {
        EmitDrops(b, 0);

        if (b->m_curRet.isVoid)
        {
            LLVMBuildRetVoid(b->m_builder);
        }
        else
        {
            LLVMBuildRet(b->m_builder, LLVMConstNull(b->m_curRetAbi));
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
        ConstValueSlot* cs = (ConstValueSlot*)StrMapGet(&b->m_constValues, n->name);

        if (cs)
        {
            /* Manifest constant: inline the value, no load. */
            return ValueMake(cs->constant, cs->td);
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

    LLVMValueRef v = LLVMBuildLoad2(b->m_builder, sym->typeDesc.type, sym->value, "id");

    return ValueMake(v, sym->typeDesc);
}

/* Re-resolves the lvalue of a moved-from node purely to store NULL into it.
   Suppresses the OOB dummy re-construction for array-index sources: the
   scratch's drop+re-init would free a dummy that was legitimately moved out
   (the move loads the value but does not null the scratch itself). */
static LValue EmitLValueForNullStore(Builder* b, Node* n)
{
    b->m_nullStoreLValue = true;
    LValue src = EmitLValue(b, n);
    b->m_nullStoreLValue = false;
    return src;
}

/* Nulls the owning binding (box/string/array ident or member chain) behind a
   moved value, so the source is no longer responsible for freeing it. */
static void NullMovedSource(Builder* b, Node* n)
{
    Node* moved = (Node*)MovableBoxSourceNode(n);

    if (!moved)
    {
        return;
    }

    LValue src = EmitLValueForNullStore(b, moved);

    if (src.valid)
    {
        if (src.typeDesc.isArray)
        {
            /* Array / string slots rebind wholesale: zero the whole fat. */
            LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
        }
        else
        {
            LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), src.ptr);
        }
    }
}

/* Produces an owned value of `innerType` from an evaluated expr - the single
   point encapsulating ownership construction. Non-owning inner returned as-is;
   owning literal heap-copied, movable source taken + nulled, non-movable taken
   as-is. Used for vars, ^T inners, fields, returns. */
static LLVMValueRef EmitOwnedValue(Builder* b, Value evaluated, Node* init, const TypeName* innerType)
{
    if (!BuilderIsOwningType(b, innerType))
    {
        return evaluated.value;
    }

    if (init->kind == NodeStrLiteral)
    {
        /* A string literal evaluates to a borrowed constant fat {ptr, len};
           an owning destination gets its own heap copy (NUL at [len]). */
        LLVMValueRef srcPtr = LLVMBuildExtractValue(b->m_builder, evaluated.value, 0, "slit.p");
        LLVMValueRef lenVal = LLVMBuildExtractValue(b->m_builder, evaluated.value, 1, "slit.l");

        return BuildOwnedStringFat(b, srcPtr, lenVal);
    }

    NullMovedSource(b, init);
    return evaluated.value;
}

/* Allocates a fresh T cell and constructs the owned inner value `src` into it,
   returning the heap pointer. Shared by every ^T / T? box-up site. */
static LLVMValueRef EmitBoxCell(Builder* b, const TypeName* innerTn, Value src, Node* srcNode, const char* name)
{
    TypeDesc innerTd = Resolve(b, innerTn);
    LLVMValueRef size = SizeOfConst(b, innerTd.type);
    LLVMValueRef args[1] = {size};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, name);
    LLVMValueRef inner = EmitOwnedValue(b, src, srcNode, innerTn);
    LLVMBuildStore(b->m_builder, inner, heap);
    return heap;
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
            none.typeDesc = TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL); /* ulong */
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

        if (base.typeDesc.isBox)
        {
            if (!base.typeDesc.boxInner)
            {
                return none;
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

        if (!base.valid || !base.typeDesc.isArray)
        {
            if (!base.valid || !base.typeDesc.isFixedArray)
            {
                return none;
            }

            /* Fixed inline array: GEP into the [N x T]; length is the compile-time dimension. */
            TypeDesc elemTd = Resolve(b, base.typeDesc.arrayInner);
            LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

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
        LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

        LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, ix->index));
        LLVMValueRef lenVal = LLVMBuildLoad2(b->m_builder, I64Ty(b), ArrayLenPtr(b, base.ptr), "len");

        none.valid = true;
        none.ptr = EmitCheckedElemPtr(b, idxVal, lenVal, dataPtr, elemTy, elemTd, base.typeDesc.aliasedArray);
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
        LLVMValueRef v = LLVMBuildLoad2(b->m_builder, lvalue.typeDesc.type, lvalue.ptr, "m");

        return ValueMake(v, lvalue.typeDesc);
    }

    Value base = EmitExpr(b, n->base_node);

    /* array.length on a non-lvalue array (e.g. MakeArr().length). */
    if (base.typeDesc.isArray && strcmp(n->member, "length") == 0)
    {
        LLVMValueRef len = LLVMBuildExtractValue(b->m_builder, base.value, 1, "len");
        return ValueMake(len, TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL));
    }

    /* Fixed-array .length: the compile-time dimension as a constant. */
    if (base.typeDesc.isFixedArray && strcmp(n->member, "length") == 0)
    {
        LLVMValueRef len = LLVMConstInt(I64Ty(b), (unsigned long long)base.typeDesc.fixedLength, 0);
        return ValueMake(len, TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL));
    }

    if (base.typeDesc.isBox && base.typeDesc.boxInner)
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

            return ValueMake(v, fieldTypeDesc);
        }
    }

    if (base.typeDesc.isBox && base.typeDesc.boxInner && IsSimdVector(base.typeDesc.boxInner->name))
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
            return (Value){dsValue, TypeDescMake(floatType, TD_FLOAT | TD_VECTOR, NULL)};
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

            LLVMValueRef v = LLVMBuildExtractValue(b->m_builder, base.value, PhysicalFieldIndex(st, idx), "m");

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

    if (!base.typeDesc.isArray)
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
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

    LLVMValueRef idxVal = AsI64Index(b, EmitExpr(b, n->index));
    LLVMValueRef lenVal = LLVMBuildExtractValue(b->m_builder, base.value, 1, "len");

    LLVMValueRef elemAddr = EmitCheckedElemPtr(b, idxVal, lenVal, dataPtr, elemTy, elemTd, false);
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
        LLVMValueRef ref = e.typeDesc.isFloat ? LLVMBuildFNeg(b->m_builder, e.value, "neg")
                                              : LLVMBuildNeg(b->m_builder, e.value, "neg");

        return ValueMake(ref, e.typeDesc);
    }

    case UnNot:
    {
        LLVMValueRef ref = LLVMBuildXor(b->m_builder, e.value, LLVMConstInt(I1Ty(b), 1, 0), "not");

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
                cond = LLVMBuildFCmp(b->m_builder, LLVMRealONE, l.value, LLVMConstReal(l.typeDesc.type, 0.0), "tobool");
            }
            else
            {
                cond = LLVMBuildICmp(b->m_builder, LLVMIntNE, l.value, LLVMConstInt(l.typeDesc.type, 0, 0), "tobool");
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
                rhsCond
                    = LLVMBuildFCmp(b->m_builder, LLVMRealONE, r.value, LLVMConstReal(r.typeDesc.type, 0.0), "tobool");
            }
            else
            {
                rhsCond
                    = LLVMBuildICmp(b->m_builder, LLVMIntNE, r.value, LLVMConstInt(r.typeDesc.type, 0, 0), "tobool");
            }
        }

        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(b->m_builder);
        LLVMBuildBr(b->m_builder, mergeBlock);

        LLVMPositionBuilderAtEnd(b->m_builder, mergeBlock);
        LLVMValueRef phi = LLVMBuildPhi(b->m_builder, I1Ty(b), "logic");

        LLVMValueRef shortCircuit = isAnd ? LLVMConstNull(I1Ty(b)) : LLVMConstInt(I1Ty(b), 1, 0);

        LLVMBasicBlockRef incomingBlocks[2] = {lhsEnd, rhsEnd};
        LLVMValueRef incomingVals[2] = {shortCircuit, rhsCond};
        LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);

        return ValueMake(phi, TypeDescMake(I1Ty(b), 0, NULL));
    }

    Value l = DerefBoxValue(b, EmitExpr(b, n->lhs));
    Value r = DerefBoxValue(b, EmitExpr(b, n->rhs));

    TypeDesc typeDesc = l.typeDesc;

    /* String equality/inequality compares the data pointers (identity,
       matching the previous representation's semantics). Other comparisons
       on strings are meaningless. */
    if (l.typeDesc.isString || r.typeDesc.isString)
    {
        if (n->op == BinEqEq || n->op == BinNotEq)
        {
            LLVMValueRef lp = LLVMBuildExtractValue(b->m_builder, l.value, 0, "seq.l");
            LLVMValueRef rp = LLVMBuildExtractValue(b->m_builder, r.value, 0, "seq.r");
            LLVMValueRef cmp = LLVMBuildICmp(b->m_builder, n->op == BinEqEq ? LLVMIntEQ : LLVMIntNE, lp, rp, "seq");

            return ValueMake(cmp, TypeDescMake(I1Ty(b), 0, NULL));
        }

        DiagError(b->m_diag, n->base.range, "strings only support '==' and '!=' comparisons");

        return ValueMake(LLVMConstNull(I1Ty(b)), TypeDescMake(I1Ty(b), 0, NULL));
    }

    if (l.typeDesc.isFloat && !r.typeDesc.isFloat)
    {
        r = Coerce(b, r, l.typeDesc);
    }
    else if (!l.typeDesc.isFloat && r.typeDesc.isFloat)
    {
        l = Coerce(b, l, r.typeDesc);

        typeDesc = r.typeDesc;
    }
    else if (!l.typeDesc.isFloat && !r.typeDesc.isFloat && l.typeDesc.type != r.typeDesc.type && l.typeDesc.type
             && r.typeDesc.type)
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

    TypeDesc boolTypeDesc = TypeDescMake(I1Ty(b), 0, NULL);

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
            typeDesc = TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL);
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
        out = flt ? LLVMBuildFAdd(b->m_builder, l.value, r.value, "add")
                  : LLVMBuildAdd(b->m_builder, l.value, r.value, "add");

        return ValueMake(out, typeDesc);

    case BinSub:
        out = flt ? LLVMBuildFSub(b->m_builder, l.value, r.value, "sub")
                  : LLVMBuildSub(b->m_builder, l.value, r.value, "sub");

        return ValueMake(out, typeDesc);

    case BinMul:
        out = flt ? LLVMBuildFMul(b->m_builder, l.value, r.value, "mul")
                  : LLVMBuildMul(b->m_builder, l.value, r.value, "mul");

        return ValueMake(out, typeDesc);

    case BinDiv:
        out = flt ? LLVMBuildFDiv(b->m_builder, l.value, r.value, "div")
                  : (typeDesc.isUnsigned ? LLVMBuildUDiv(b->m_builder, l.value, r.value, "div")
                                         : LLVMBuildSDiv(b->m_builder, l.value, r.value, "div"));

        return ValueMake(out, typeDesc);

    case BinMod:
        out = flt ? LLVMBuildFRem(b->m_builder, l.value, r.value, "mod")
                  : (typeDesc.isUnsigned ? LLVMBuildURem(b->m_builder, l.value, r.value, "mod")
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
        out = typeDesc.isUnsigned ? LLVMBuildLShr(b->m_builder, l.value, r.value, "shr")
                                  : LLVMBuildAShr(b->m_builder, l.value, r.value, "shr");

        return ValueMake(out, typeDesc);

    case BinEqEq:
        out = flt ? FcmpByName(b->m_builder, "oeq", l.value, r.value)
                  : IcmpByName(b->m_builder, "eq", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinNotEq:
        out = flt ? FcmpByName(b->m_builder, "one", l.value, r.value)
                  : IcmpByName(b->m_builder, "ne", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLt:
        out = flt ? FcmpByName(b->m_builder, "olt", l.value, r.value)
                  : IcmpByName(b->m_builder, "lt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinLtEq:
        out = flt ? FcmpByName(b->m_builder, "ole", l.value, r.value)
                  : IcmpByName(b->m_builder, "le", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGt:
        out = flt ? FcmpByName(b->m_builder, "ogt", l.value, r.value)
                  : IcmpByName(b->m_builder, "gt", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    case BinGtEq:
        out = flt ? FcmpByName(b->m_builder, "oge", l.value, r.value)
                  : IcmpByName(b->m_builder, "ge", typeDesc.isUnsigned, l.value, r.value);

        return ValueMake(out, boolTypeDesc);

    default:
        return l;
    }
}

static Value EmitAssign(Builder* b, AssignExpr* n)
{
    /* Impl property write (`cam.FOV = v`): lower to the setter extern call
       (compound assigns are rejected in sema). */
    if (n->target->kind == NodeMember && n->op == AssignSet)
    {
        bool isProp = false;
        Value prop = EmitImplProperty(b, (MemberExpr*)n->target, n->value, &isProp);

        if (isProp)
        {
            return prop;
        }
    }

    Value rhs = EmitExpr(b, n->value);

    if (n->target->kind == NodeIdent || n->target->kind == NodeMember || n->target->kind == NodeIndex)
    {
        LValue lvalue = EmitLValue(b, n->target);

        if (lvalue.valid)
        {
            /* Whole-array / string rebind: free the old buffer, take the new
               {ptr,len} struct, and null a moved source. A string literal RHS
               is a borrowed constant, so it is heap-copied first. */
            if (lvalue.typeDesc.isArray && n->op == AssignSet)
            {
                if (lvalue.typeDesc.isString && n->value->kind == NodeStrLiteral)
                {
                    LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, rhs.value, 0, "aslit.p");
                    LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, rhs.value, 1, "aslit.l");
                    rhs = ValueMake(BuildOwnedStringFat(b, sp, sl), rhs.typeDesc);
                }

                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                Node* movedNode = (Node*)MovableBoxSourceNode(n->value);

                if (movedNode)
                {
                    LValue src = EmitLValueForNullStore(b, movedNode);

                    if (src.valid && src.typeDesc.isArray)
                    {
                        LLVMBuildStore(b->m_builder, LLVMConstNull(ArrayStructType(b)), src.ptr);
                    }
                }

                return ValueMake(rhs.value, lvalue.typeDesc);
            }

            /* `=` rebinds the box only when RHS is a box of the SAME kind
               (string=string, ^T=^T); any other assign into a ^T - a plain
               T value (`x = 5;`) or a string into ^string - mutates its
               contents instead. */
            bool rhsIsSameBoxKind
                = rhs.typeDesc.isBox
                  && ((lvalue.typeDesc.boxInner == NULL && rhs.typeDesc.boxInner == NULL)
                      || (lvalue.typeDesc.boxInner && rhs.typeDesc.boxInner
                          && strcmp(lvalue.typeDesc.boxInner->name, rhs.typeDesc.boxInner->name) == 0));
            bool boxMove = lvalue.typeDesc.isBox && n->op == AssignSet && rhsIsSameBoxKind;
            bool boxMoveFromLiteral = boxMove && !lvalue.typeDesc.boxInner && n->value->kind == NodeStrLiteral;

            if (boxMove && !boxMoveFromLiteral)
            {
                /* Box move: take the pointer, null the source, drop the old
                   value, rebind. The SOURCE is detached BEFORE the drop so the
                   drop glue (and the store) never touch freed memory - for
                   `cur = cur.next` the source slot lives inside the freed object. */
                Node* movedSourceNodePre = (Node*)MovableBoxSourceNode(n->value);
                LValue srcPre = movedSourceNodePre ? EmitLValueForNullStore(b, movedSourceNodePre) : (LValue){0};

                if (srcPre.valid)
                {
                    LLVMBuildStore(b->m_builder, LLVMConstNull(b->m_ptrTy), srcPre.ptr);
                }

                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                LLVMBuildStore(b->m_builder, rhs.value, lvalue.ptr);

                return rhs;
            }

            if (boxMoveFromLiteral)
            {
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);
                StrLiteral* lit = AsNode(StrLiteral, n->value);
                size_t copyLen = strlen(lit->value) + 1;
                LLVMValueRef size = LLVMConstInt(I64Ty(b), (unsigned long long)copyLen, 0);
                LLVMValueRef args2[1] = {size};
                StrataAllocFn(b);
                LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args2, 1, "stra");
                LLVMBuildStore(b->m_builder, heap, lvalue.ptr);
                LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
                LLVMValueRef srcGep = rhs.value;
                LLVMValueRef dstGep = heap;
                for (size_t ci = 0; ci < copyLen; ci++)
                {
                    LLVMValueRef ciVal = LLVMConstInt(I64Ty(b), (unsigned long long)ci, 0);
                    LLVMValueRef srcIdx[1] = {ciVal};
                    LLVMValueRef dstIdx[1] = {ciVal};
                    LLVMValueRef byteVal = LLVMBuildLoad2(
                        b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, srcGep, srcIdx, 1, "srcas"), "bas");
                    LLVMBuildStore(b->m_builder, byteVal,
                                   LLVMBuildGEP2(b->m_builder, i8Ty, dstGep, dstIdx, 1, "dstas"));
                }

                return rhs;
            }

            /* Optional (`T?`) slot receiving a NON-box value: whole-slot
               rebind. The old value is dropped (EmitDropOne handles empty/
               NULL), then a fresh cell is allocated and the owned inner
               constructed into it. Same-kind box RHS took the boxMove path. */
            bool optRebind = lvalue.typeDesc.isOptional && n->op == AssignSet;

            if (optRebind)
            {
                /* Drop whatever the slot held first (no-op when empty). */
                EmitDropOne(b, lvalue.ptr, lvalue.typeDesc);

                const TypeName* innerTn = lvalue.typeDesc.boxInner ? lvalue.typeDesc.boxInner : StringTypeName(b);
                TypeDesc innerTd = Resolve(b, innerTn);

                if (!innerTd.isArray && !innerTd.isBox)
                {
                    /* Scalar / struct / handle inner: allocate a cell and
                       construct the owned value into it. */
                    LLVMValueRef size = SizeOfConst(b, innerTd.type);
                    LLVMValueRef args[1] = {size};
                    StrataAllocFn(b);
                    LLVMValueRef cell
                        = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "optcell");
                    LLVMValueRef inner = Coerce(b, rhs, innerTd).value;
                    LLVMBuildStore(b->m_builder, inner, cell);
                    LLVMBuildStore(b->m_builder, cell, lvalue.ptr);

                    return ValueMake(cell, lvalue.typeDesc);
                }

                /* String inner: construct the owned string (heap copy), then
                   wrap it in a cell - the boxed representation is slot -> cell
                   -> chars, as drop glue and reads expect. Array inner cannot
                   occur (optionals of dynamic arrays are rejected by sema). */
                LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, innerTn);

                LLVMValueRef cellSz = SizeOfConst(b, b->m_ptrTy);
                LLVMValueRef cellArgs[1] = {cellSz};
                StrataAllocFn(b);
                LLVMValueRef strCell
                    = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, cellArgs, 1, "optstr");
                LLVMBuildStore(b->m_builder, owned, strCell);
                LLVMBuildStore(b->m_builder, strCell, lvalue.ptr);

                return ValueMake(strCell, lvalue.typeDesc);
            }

            if (lvalue.typeDesc.isBox && !boxMove)
            {
                /* Assigning a plain T (or compound-assign) into a ^T mutates
                   its contents in place, not a move - so `x = 5;` and `val -=
                   amt;` work even through a `ref ^T` param or box global.
                   lvalue.ptr is the box slot; load it, then read/write through. */
                LLVMValueRef boxPtr = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lvalue.ptr, "box");
                const TypeName* innerTn = lvalue.typeDesc.boxInner;

                if (!innerTn)
                {
                    innerTn = StringTypeName(b);
                }

                TypeDesc innerTd = Resolve(b, innerTn);

                if (BuilderIsOwningType(b, innerTn))
                {
                    /* Content-assigning an OWNING inner (^string = "x"): drop
                       only the old inner value (free it in place, NOT the box
                       allocation), then construct a fresh owned inner into the
                       existing box: heap-copy a literal, move a movable source. */
                    EmitDropOne(b, boxPtr, innerTd);
                    LLVMValueRef owned = EmitOwnedValue(b, rhs, n->value, innerTn);
                    LLVMBuildStore(b->m_builder, owned, boxPtr);
                    return ValueMake(owned, innerTd);
                }

                LLVMValueRef cur = LLVMBuildLoad2(b->m_builder, innerTd.type, boxPtr, "boxval");
                Value result = Coerce(b, rhs, innerTd);
                bool flt = innerTd.isFloat;

                switch (n->op)
                {
                case AssignAdd:
                    result = ValueMake(flt ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                                           : LLVMBuildAdd(b->m_builder, cur, result.value, "add"),
                                       innerTd);
                    break;
                case AssignSub:
                    result = ValueMake(flt ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                                           : LLVMBuildSub(b->m_builder, cur, result.value, "sub"),
                                       innerTd);
                    break;
                case AssignMul:
                    result = ValueMake(flt ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                                           : LLVMBuildMul(b->m_builder, cur, result.value, "mul"),
                                       innerTd);
                    break;
                case AssignDiv:
                    result
                        = ValueMake(flt ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                                        : (innerTd.isUnsigned ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                                                              : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")),
                                    innerTd);
                    break;
                case AssignMod:
                    result
                        = ValueMake(flt ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                                        : (innerTd.isUnsigned ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                                                              : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")),
                                    innerTd);
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
                    result = ValueMake(flt ? LLVMBuildFAdd(b->m_builder, cur, result.value, "add")
                                           : LLVMBuildAdd(b->m_builder, cur, result.value, "add"),
                                       lvalue.typeDesc);
                    break;
                case AssignSub:
                    result = ValueMake(flt ? LLVMBuildFSub(b->m_builder, cur, result.value, "sub")
                                           : LLVMBuildSub(b->m_builder, cur, result.value, "sub"),
                                       lvalue.typeDesc);
                    break;
                case AssignMul:
                    result = ValueMake(flt ? LLVMBuildFMul(b->m_builder, cur, result.value, "mul")
                                           : LLVMBuildMul(b->m_builder, cur, result.value, "mul"),
                                       lvalue.typeDesc);
                    break;
                case AssignDiv:
                    result = ValueMake(flt ? LLVMBuildFDiv(b->m_builder, cur, result.value, "div")
                                           : (lvalue.typeDesc.isUnsigned
                                                  ? LLVMBuildUDiv(b->m_builder, cur, result.value, "div")
                                                  : LLVMBuildSDiv(b->m_builder, cur, result.value, "div")),
                                       lvalue.typeDesc);
                    break;
                case AssignMod:
                    result = ValueMake(flt ? LLVMBuildFRem(b->m_builder, cur, result.value, "mod")
                                           : (lvalue.typeDesc.isUnsigned
                                                  ? LLVMBuildURem(b->m_builder, cur, result.value, "mod")
                                                  : LLVMBuildSRem(b->m_builder, cur, result.value, "mod")),
                                       lvalue.typeDesc);
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
static void EmitArrayCopyLoop(Builder* b, LLVMValueRef dstData, LLVMValueRef srcData, LLVMValueRef count,
                              LLVMTypeRef elemTy)
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
    LLVMValueRef ep[1] = {i};
    LLVMValueRef srcEl = LLVMBuildGEP2(b->m_builder, elemTy, srcData, ep, 1, "s");
    LLVMValueRef dstEl = LLVMBuildGEP2(b->m_builder, elemTy, dstData, ep, 1, "d");
    LLVMBuildStore(b->m_builder, LLVMBuildLoad2(b->m_builder, elemTy, srcEl, "e"), dstEl);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, end);
}

/* Zero-initializes elements [start, end) of data (nulls owning slots safely). */
static void EmitArrayZeroLoop(Builder* b, LLVMValueRef data, LLVMValueRef start, LLVMValueRef end, LLVMTypeRef elemTy)
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
    LLVMValueRef ep[1] = {i};
    LLVMValueRef el = LLVMBuildGEP2(b->m_builder, elemTy, data, ep, 1, "z");
    LLVMBuildStore(b->m_builder, LLVMConstNull(elemTy), el);
    LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
    LLVMBuildStore(b->m_builder, next, iSlot);
    Br(b, cond);

    PositionAtEnd(b, endBb);
}

/* Drops owning elements [start, end) of data before the buffer is freed. */
static void EmitArrayDropRange(Builder* b, LLVMValueRef data, LLVMValueRef start, LLVMValueRef end, TypeDesc elemTd,
                               LLVMTypeRef elemTy)
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
    LLVMValueRef ep[1] = {i};
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

    TypeDesc elemTd = Resolve(b, arr.typeDesc.arrayInner);
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;
    TypeDesc ulongTd = TypeDescMake(I64Ty(b), TD_UNSIGNED, NULL);

    LLVMValueRef dataPtrPtr = ArrayDataPtr(b, arr.ptr);
    LLVMValueRef lenPtr = ArrayLenPtr(b, arr.ptr);

    if (strcmp(n->callee, "array_pop") == 0)
    {
        LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I64Ty(b), lenPtr, "len");
        LLVMValueRef one = LLVMConstInt(I64Ty(b), 1, 0);
        LLVMValueRef lm1 = LLVMBuildSub(b->m_builder, len, one, "lm1");
        LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataPtrPtr, "data");
        LLVMValueRef idx[1] = {lm1};
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
        LLVMValueRef allocArgs[1] = {totalBytes};
        StrataAllocFn(b);
        LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "pushbuf");

        EmitArrayCopyLoop(b, newData, oldData, oldLen, elemTy);

        Node* valNode = (Node*)VecGet(&n->args, 1);
        LLVMValueRef slotIdx[1] = {oldLen};
        LLVMValueRef elAddr = LLVMBuildGEP2(b->m_builder, elemTy, newData, slotIdx, 1, "pushslot");
        Value v = EmitExpr(b, valNode);

        bool sameOwningType = v.typeDesc.isBox
                              && ((elemTd.boxInner == NULL && v.typeDesc.boxInner == NULL)
                                  || (elemTd.boxInner && v.typeDesc.boxInner
                                      && strcmp(elemTd.boxInner->name, v.typeDesc.boxInner->name) == 0))
                              && valNode->kind != NodeStrLiteral;

        bool stringMove = elemTd.isString && v.typeDesc.isString && valNode->kind != NodeStrLiteral;

        if (sameOwningType || stringMove)
        {
            LLVMBuildStore(b->m_builder, v.value, elAddr);
            NullMovedSource(b, valNode);
        }
        else if (elemTd.isBox && elemTd.boxInner)
        {
            /* ^T element from a non-^T value (bare T, string literal
               into ^string): allocate a T slot, construct the owned
               inner value, store it. */
            TypeDesc innerTd = Resolve(b, elemTd.boxInner);
            LLVMValueRef sz = SizeOfConst(b, innerTd.type);
            LLVMValueRef args[1] = {sz};
            StrataAllocFn(b);
            LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "abox");
            LLVMValueRef inner = EmitOwnedValue(b, v, valNode, elemTd.boxInner);
            LLVMBuildStore(b->m_builder, inner, heap);
            LLVMBuildStore(b->m_builder, heap, elAddr);
        }
        else if (elemTd.isString)
        {
            /* string element: heap-copy literal, move source. */
            LLVMValueRef owned = EmitOwnedValue(b, v, valNode, StringTypeName(b));
            LLVMBuildStore(b->m_builder, owned, elAddr);
        }
        else
        {
            LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, elAddr);
        }

        LLVMValueRef freeArgs[1] = {oldData};
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
    LLVMValueRef allocArgs[1] = {allocBytes};
    StrataAllocFn(b);
    LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "rszbuf");

    EmitArrayCopyLoop(b, newData, oldData, copyCount, elemTy);
    EmitArrayZeroLoop(b, newData, copyCount, newLen, elemTy);

    if (arr.typeDesc.arrayInner && BuilderIsOwningType(b, arr.typeDesc.arrayInner))
    {
        EmitArrayDropRange(b, oldData, newLen, oldLen, elemTd, elemTy);
    }

    LLVMValueRef freeArgs[1] = {oldData};
    StrataFreeFn(b);
    LLVMBuildCall2(b->m_builder, b->m_freeFnType, b->m_freeFn, freeArgs, 1, "");

    LLVMBuildStore(b->m_builder, newData, dataPtrPtr);
    LLVMBuildStore(b->m_builder, newLen, lenPtr);

    return ValueMake(NULL, TypeDescMake(NULL, TD_VOID, NULL));
}

/* Emits the body of strata_strdup. Uses host defined strata_jit and strata_free */
static void EmitStrataStrdupBody(Builder* b)
{
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(b->m_builder);

    LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
    LLVMTypeRef i64Ty = I64Ty(b);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "entry");
    LLVMBasicBlockRef lenCond = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "len.cond");
    LLVMBasicBlockRef lenBody = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "len.body");
    LLVMBasicBlockRef allocBB = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "alloc");
    LLVMBasicBlockRef copyCond = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "copy.cond");
    LLVMBasicBlockRef copyBody = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "copy.body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(b->m_ctx, b->m_strdupFn, "done");

    LLVMValueRef s = LLVMGetParam(b->m_strdupFn, 0);
    LLVMValueRef zero = LLVMConstInt(i64Ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(i64Ty, 1, 0);

    /* Both loop counters are allocas in the ENTRY block - an alloca in a
       non-entry block makes LLVM's x86 codegen treat the frame as dynamic and
       emit a __chkstk stack probe. */
    LLVMPositionBuilderAtEnd(b->m_builder, entry);
    LLVMValueRef iSlot = LLVMBuildAlloca(b->m_builder, i64Ty, "i");
    LLVMValueRef jSlot = LLVMBuildAlloca(b->m_builder, i64Ty, "j");
    LLVMBuildStore(b->m_builder, zero, iSlot);
    LLVMBuildBr(b->m_builder, lenCond);

    LLVMPositionBuilderAtEnd(b->m_builder, lenCond);
    LLVMValueRef i = LLVMBuildLoad2(b->m_builder, i64Ty, iSlot, "i");
    LLVMValueRef si[1] = {i};
    LLVMValueRef c = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, s, si, 1, "si"), "c");
    LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, c, LLVMConstNull(i8Ty), "isn");
    LLVMBuildCondBr(b->m_builder, isNull, allocBB, lenBody);

    LLVMPositionBuilderAtEnd(b->m_builder, lenBody);
    LLVMBuildStore(b->m_builder, LLVMBuildAdd(b->m_builder, i, one, "i1"), iSlot);
    LLVMBuildBr(b->m_builder, lenCond);

    LLVMPositionBuilderAtEnd(b->m_builder, allocBB);
    LLVMValueRef len = LLVMBuildLoad2(b->m_builder, i64Ty, iSlot, "n");
    LLVMValueRef allocArgs[1] = {LLVMBuildAdd(b->m_builder, len, one, "size")};
    StrataAllocFn(b);
    LLVMValueRef d = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "d");
    LLVMBuildStore(b->m_builder, zero, jSlot);
    LLVMBuildBr(b->m_builder, copyCond);

    LLVMPositionBuilderAtEnd(b->m_builder, copyCond);
    LLVMValueRef j = LLVMBuildLoad2(b->m_builder, i64Ty, jSlot, "j");
    LLVMValueRef jLe = LLVMBuildICmp(b->m_builder, LLVMIntULE, j, len, "jle");
    LLVMBuildCondBr(b->m_builder, jLe, copyBody, done);

    LLVMPositionBuilderAtEnd(b->m_builder, copyBody);
    LLVMValueRef sj[1] = {j};
    LLVMValueRef dAddr = LLVMBuildGEP2(b->m_builder, i8Ty, d, sj, 1, "dj");
    LLVMValueRef byte = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, s, sj, 1, "sj"), "b");
    LLVMBuildStore(b->m_builder, byte, dAddr);
    LLVMBuildStore(b->m_builder, LLVMBuildAdd(b->m_builder, j, one, "j1"), jSlot);
    LLVMBuildBr(b->m_builder, copyCond);

    LLVMPositionBuilderAtEnd(b->m_builder, done);
    LLVMBuildRet(b->m_builder, d);

    if (savedBlock)
    {
        LLVMPositionBuilderAtEnd(b->m_builder, savedBlock);
    }
}

static LLVMValueRef StrataStrdupFn(Builder* b)
{
    if (!b->m_strdupFn)
    {
        LLVMTypeRef params[1] = {b->m_ptrTy};
        b->m_strdupFnType = LLVMFunctionType(b->m_ptrTy, params, 1, 0);
        b->m_strdupFn = LLVMAddFunction(b->m_mod, "strata_strdup", b->m_strdupFnType);
        EmitStrataStrdupBody(b);
    }
    return b->m_strdupFn;
}

/* Deep-copies the CELL behind a non-null box pointer `src` into a fresh
   allocation, returning the new cell pointer. Callers guard optionals. */
static Value EmitCopyValue(Builder* b, Value src, TypeDesc td);

/* Copies a non-empty fat array value: fresh buffer + per-element deep copy
   for owning element types, memcpy-style loop otherwise. Callers guard
   empty optionals. */
static Value EmitArrayValueCopy(Builder* b, Value src, TypeDesc td, TypeDesc elemTd, LLVMTypeRef elemTy,
                                LLVMValueRef oldLen, LLVMValueRef oldData)
{
    LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, SizeOfConst(b, elemTy), oldLen, "cby");
    LLVMValueRef allocArgs[1] = {totalBytes};
    StrataAllocFn(b);
    LLVMValueRef newData = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "cpya");

    if (BuilderIsOwningType(b, td.arrayInner))
    {
        LLVMBasicBlockRef cond = NewBb(b, "ccp.cond");
        LLVMBasicBlockRef body = NewBb(b, "ccp.body");
        LLVMBasicBlockRef end = NewBb(b, "ccp.end");
        LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "ccp.i");
        LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
        Br(b, cond);

        PositionAtEnd(b, cond);
        LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
        LLVMValueRef cnt = LLVMBuildICmp(b->m_builder, LLVMIntULT, i, oldLen, "cclt");
        LLVMBuildCondBr(b->m_builder, cnt, body, end);
        b->m_terminated = true;

        PositionAtEnd(b, body);
        LLVMValueRef ep[1] = {i};
        LLVMValueRef srcElem = LLVMBuildGEP2(b->m_builder, elemTy, oldData, ep, 1, "cse");
        LLVMValueRef dstElem = LLVMBuildGEP2(b->m_builder, elemTy, newData, ep, 1, "cde");
        Value elemVal = ValueMake(LLVMBuildLoad2(b->m_builder, elemTy, srcElem, "ev"), elemTd);
        Value copied = EmitCopyValue(b, elemVal, elemTd);
        LLVMBuildStore(b->m_builder, copied.value, dstElem);
        LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "nxt");
        LLVMBuildStore(b->m_builder, next, iSlot);
        Br(b, cond);

        PositionAtEnd(b, end);
    }
    else
    {
        EmitArrayCopyLoop(b, newData, oldData, oldLen, elemTy);
    }

    LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
    arr = LLVMBuildInsertValue(b->m_builder, arr, newData, 0, "ci");
    arr = LLVMBuildInsertValue(b->m_builder, arr, oldLen, 1, "cl");
    return ValueMake(arr, td);
}

static LLVMValueRef EmitBoxCellCopy(Builder* b, Value src, TypeDesc td)
{
    TypeDesc innerTd = Resolve(b, td.boxInner);
    LLVMValueRef sz = SizeOfConst(b, innerTd.type);
    LLVMValueRef args[1] = {sz};
    StrataAllocFn(b);
    LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "cpbox");

    const StructType* st = TypeRegistryFind(&b->m_registry, td.boxInner->name);
    LLVMTypeRef structTy = st ? (LLVMTypeRef)StrMapGet(&b->m_structTypes, td.boxInner->name) : NULL;

    if (st && structTy)
    {
        for (size_t j = 0; j < st->fields.count; j++)
        {
            FieldDecl* f = (FieldDecl*)VecGet(&st->fields, j);
            TypeDesc fieldTd = Resolve(b, &f->type);
            LLVMValueRef idxs[2] = {IdxConst(b, 0), IdxConst(b, PhysicalFieldIndex(st, (int)j))};
            LLVMValueRef srcField = LLVMBuildGEP2(b->m_builder, structTy, src.value, idxs, 2, "csf");
            LLVMValueRef dstField = LLVMBuildGEP2(b->m_builder, structTy, heap, idxs, 2, "cdf");

            if (BuilderIsOwningType(b, &f->type))
            {
                Value fv = ValueMake(LLVMBuildLoad2(b->m_builder, fieldTd.type, srcField, "fv"), fieldTd);
                Value copied = EmitCopyValue(b, fv, fieldTd);
                LLVMBuildStore(b->m_builder, copied.value, dstField);
            }
            else
            {
                LLVMValueRef loaded = LLVMBuildLoad2(b->m_builder, fieldTd.type, srcField, "fv");
                LLVMBuildStore(b->m_builder, loaded, dstField);
            }
        }
    }
    else
    {
        /* Inner is not a registered struct: a scalar, or a bare owning
            value such as string (^string), another box, or an array.
            Deep-copy it. */
        Value innerVal = ValueMake(LLVMBuildLoad2(b->m_builder, innerTd.type, src.value, "cv"), innerTd);
        Value copied = EmitCopyValue(b, innerVal, innerTd);
        LLVMBuildStore(b->m_builder, copied.value, heap);
    }

    return heap;
}

static Value EmitCopyValue(Builder* b, Value src, TypeDesc td)
{
    if (td.isString)
    {
        /* Deep copy: fresh buffer with the NUL terminator at [len]. */
        LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, src.value, 0, "cp.p");
        LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, src.value, 1, "cp.l");

        return ValueMake(BuildOwnedStringFat(b, sp, sl), td);
    }

    if (td.isBox && !td.boxInner)
    {
        StrataStrdupFn(b);
        LLVMValueRef args[1] = {src.value};
        LLVMValueRef copy = LLVMBuildCall2(b->m_builder, b->m_strdupFnType, b->m_strdupFn, args, 1, "cpstr");
        return ValueMake(copy, td);
    }

    if (td.isBox && td.boxInner)
    {
        /* An optional source may be EMPTY (null): copy null as null instead
           of dereferencing it. */
        if (td.isOptional)
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

    if (td.isArray)
    {
        TypeDesc elemTd = Resolve(b, td.arrayInner);
        LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;
        LLVMValueRef oldLen = LLVMBuildExtractValue(b->m_builder, src.value, 1, "cln");
        LLVMValueRef oldData = LLVMBuildExtractValue(b->m_builder, src.value, 0, "cdt");

        /* Copying an EMPTY optional array yields the canonical empty
           {null, 0} - never a {malloc(0), 0} that would test non-empty. */
        if (td.isOptional)
        {
            LLVMValueRef isEmpty = LLVMBuildICmp(b->m_builder, LLVMIntEQ, oldLen, LLVMConstInt(I64Ty(b), 0, 0), "oae");
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
static Value EmitCopyBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    Value v = EmitExpr(b, arg0);

    return EmitCopyValue(b, v, v.typeDesc);
}

/* Emits drop(arg) — destroys the owning argument immediately, as if it were
   moved into an empty `void drop(^T v) {}` whose owned param is dropped at
   scope exit. Although it works for un-blessed optionals too */
static Value EmitDropBuiltin(Builder* b, CallExpr* n)
{
    Node* arg0 = (Node*)VecGet(&n->args, 0);
    LLVMTypeRef voidTy = LLVMVoidTypeInContext(b->m_ctx);

    /* Drop the owning lvalue in place (frees recursively, then nulls the slot). */
    Node* moved = (Node*)MovableBoxSourceNode(arg0);

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
    LLVMValueRef slot = EntryAlloca(b, v.typeDesc.isArray ? ArrayStructType(b) : b->m_ptrTy, "drop.tmp");
    LLVMBuildStore(b->m_builder, v.value, slot);
    EmitDropOne(b, slot, v.typeDesc);

    return ValueMake(LLVMGetUndef(voidTy), TypeDescMake(voidTy, TD_VOID, NULL));
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

static Value EmitVectorConstruct(Builder* b, CallExpr* n)
{
    TypeName tn = MakeTypeName(b, n->callee);
    TypeDesc typeDesc = Resolve(b, &tn);

    int numVectorLanes = IsSimdVector(n->callee);

    Value v;

    /* Float2 */
    if (numVectorLanes == 2)
    {
        v.value = LSimdVector2Construct(b, n);
    }
    /* If we have a vector3 or vector4, treat it like a vector4 (float128). */
    else if (numVectorLanes >= 3)
    {
        v.value = LSimdVector4Construct(b, n);
    }

    if (!v.value)
    {
        v.value = LLVMGetPoison(typeDesc.type);
    }
    v.typeDesc = typeDesc;

    return v;
}

static Value EmitVectorDot(Builder* b, CallExpr* n)
{
    Value vA = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value vB = EmitExpr(b, (Node*)VecGet(&n->args, 1));

    return ValueMake(LSimdVectorDot(b, vA.value, vB.value), ResolveByName(b, "float"));
}

static Value EmitVectorCross(Builder* b, CallExpr* n)
{
    Value vA = EmitExpr(b, (Node*)VecGet(&n->args, 0));
    Value vB = EmitExpr(b, (Node*)VecGet(&n->args, 1));

    LLVMValueRef value = LSimdVectorCross(b, vA.value, vB.value);

    TypeDesc resultTd;

    /* The cross product of float2 yields a scalar value */
    if (LLVMGetVectorSize(LLVMTypeOf(vA.value)) == 2)
    {
        resultTd = ResolveByName(b, "float");
    }
    else
    {
        /* The cross product of float3 and float4 return the same vector type as the arguments */
        resultTd = vA.typeDesc;
    }

    return ValueMake(value, resultTd);
}

typedef struct PseudoCallDefinition
{
    const char* name;
    Value (*callDef)(Builder* b, CallExpr* n);
} IntrinsicDefinition;

static Value EmitIntrinsicCall(Builder* b, CallExpr* n, bool* isValid)
{
    static const IntrinsicDefinition intrinsics[] = {
        /* Array calls */
        {"array_push",   EmitArrayBuiltin   },
        {"array_pop",    EmitArrayBuiltin   },
        {"array_resize", EmitArrayBuiltin   },

        /* Misc */
        {"copy",         EmitCopyBuiltin    },
        {"drop",         EmitDropBuiltin    },

        /* Vectors */
        {"float2",       EmitVectorConstruct},
        {"float3",       EmitVectorConstruct},
        {"float4",       EmitVectorConstruct},

        {"dot",          EmitVectorDot      },
        {"cross",        EmitVectorCross    },
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

static Value EmitCall(Builder* b, CallExpr* n)
{
    if (n->isPseudoCall)
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
            else if (fieldTd.isBox && fieldTd.boxInner
                     && !(rawArg.typeDesc.isBox && rawArg.typeDesc.boxInner
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

            agg = LLVMBuildInsertValue(b->m_builder, agg, argValue.value, PhysicalFieldIndex(st, (int)i), "ins");

            /* If an owning field was moved from an owning lvalue source,
               null the source so its scope-exit drop is a no-op. */
            if ((fieldTd.isBox || fieldTd.isString) && (rawArg.typeDesc.isBox || rawArg.typeDesc.isString)
                && argNode->kind != NodeStrLiteral)
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

                argIsBox = sym && sym->typeDesc.isBox;
            }
            else if (argNode->kind == NodeCall)
            {
                FuncInfo* fi = (FuncInfo*)StrMapGet(&b->m_funcs, ((CallExpr*)argNode)->callee);
                argIsBox = fi && fi->returnType.isBox;
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

                if (TypeNameIsDynamicArray(pty) && !(elem && BuilderIsOwningType(b, elem)))
                {
                    LLVMValueRef arrAddr = ArgAddress(b, argNode);
                    LLVMValueRef dataSlot = ArrayDataPtr(b, arrAddr);
                    args[k] = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, dataSlot, "arrptr");
                    continue;
                }
            }
        }

        if (shouldPassByPtr && paramIsBoxType && argNode->kind == NodeStrLiteral)
        {
            /* Internal string param (by slot address): the callee owns and
               drops the value, so materialize an owned fat into a fresh
               temp slot instead of handing out the borrowed constant. */
            Value litVal = EmitExpr(b, argNode);
            LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, litVal.value, 0, "arglit.p");
            LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, litVal.value, 1, "arglit.l");
            LLVMValueRef owned = BuildOwnedStringFat(b, sp, sl);

            LLVMValueRef slot = EntryAlloca(b, ArrayStructType(b), "stslot");
            LLVMBuildStore(b->m_builder, owned, slot);

            args[k] = slot;
        }
        else if (shouldPassByPtr && !paramIsBoxType && argIsBox)
        {
            args[k] = EmitExpr(b, argNode).value;
        }
        else if (!shouldPassByPtr && paramIsBoxType && fd && fd->isExtern && k < fd->params.count)
        {
            /* Extern ^T / T? param: pass the pointer ITSELF by value.
               A box/optional arg already is that pointer; a plain owning
               value is constructed into a fresh cell first. */
            const ParamDecl* extParam = (ParamDecl*)VecGet(&fd->params, k);
            const TypeName* extTy = &extParam->type;

            Value av = EmitExpr(b, argNode);

            /* deref */
            bool paramIsRealBox = extTy->isBox || extTy->isOptional;

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
                    args[k] = BuildExternStringPtr(b, av);
                }
            }
            else if (av.typeDesc.isBox)
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
                args[k] = ApplyCVarargPromotion(b, DerefBoxValue(b, v));
            }
            else
            {
                args[k] = shouldPassByPtr ? ArgAddress(b, argNode) : DerefBoxValue(b, v).value;
            }
        }
    }

    LLVMValueRef callee = info->function;
    bool slotExtern = false;

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
        call = LLVMBuildCall2(b->m_builder, info->type, callee, args, (unsigned)nargs, "call");
    }
    else
    {
        call = LLVMBuildCall2(b->m_builder, info->type, callee, args, (unsigned)nargs, "call");
    }

    /* Return-param call: the C function wrote the result into our temp slot;
       load it back as the Strata-level return value. */
    if (hasReturnParam)
    {
        LLVMValueRef retVal = LLVMBuildLoad2(b->m_builder, returnParamTd.type, returnParamSlot, "retparam.val");

        return ValueMake(retVal, returnParamTd);
    }

    /* Extern string return: the C ABI handed back a char* (NUL-terminated
       by contract). Wrap it into a fat: substitute a valid empty buffer for
       NULL and measure the length with an inline scan. */
    if (info->externStringReturn)
    {
        LLVMValueRef isNull = LLVMBuildICmp(b->m_builder, LLVMIntEQ, call, LLVMConstNull(b->m_ptrTy), "retnull");
        LLVMValueRef dataPtr = LLVMBuildSelect(b->m_builder, isNull, EmptyNulString(b), call, "retstr");

        /* Inline strlen: len = 0 while dataPtr[len] != 0. */
        LLVMBasicBlockRef cond = NewBb(b, "slen.cond");
        LLVMBasicBlockRef body = NewBb(b, "slen.body");
        LLVMBasicBlockRef done = NewBb(b, "slen.done");
        LLVMValueRef iSlot = EntryAlloca(b, I64Ty(b), "slen.i");
        LLVMBuildStore(b->m_builder, LLVMConstInt(I64Ty(b), 0, 0), iSlot);
        Br(b, cond);

        PositionAtEnd(b, cond);
        LLVMValueRef i = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "i");
        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
        LLVMValueRef byte
            = LLVMBuildLoad2(b->m_builder, i8Ty, LLVMBuildGEP2(b->m_builder, i8Ty, dataPtr, &i, 1, "sc"), "c");
        LLVMValueRef more = LLVMBuildICmp(b->m_builder, LLVMIntNE, byte, LLVMConstNull(i8Ty), "nz");
        LLVMBuildCondBr(b->m_builder, more, body, done);
        b->m_terminated = true;

        PositionAtEnd(b, body);
        LLVMValueRef next = LLVMBuildAdd(b->m_builder, i, LLVMConstInt(I64Ty(b), 1, 0), "next");
        LLVMBuildStore(b->m_builder, next, iSlot);
        Br(b, cond);

        PositionAtEnd(b, done);
        LLVMValueRef lenVal = LLVMBuildLoad2(b->m_builder, I64Ty(b), iSlot, "len");

        return ValueMake(MakeStringFatValue(b, dataPtr, lenVal), info->returnType);
    }

    return ValueMake(call, info->returnType);
}

/* Builds a T[] from element exprs, shared by array literals (heap-backed) and
   typed rest params. rest=stack-backed (alloca in entry); borrow=true (ref
   rest) stores owning element pointers without nulling/moving sources. */
static Value EmitArrayFromNodes(Builder* b, const TypeName* elementType, const Vec* elements, bool stackBuffer,
                                bool borrow)
{
    TypeDesc elemTd = Resolve(b, elementType);

    /* The LLVM type of one element slot in the backing buffer. Arrays and
        boxes/strings both reduce to a pointer; structs/scalars use their
        own type. */
    LLVMTypeRef elemTy = elemTd.isArray ? ArrayStructType(b) : elemTd.type;

    /* A `ref T... rest` with non-owning elements holds POINTERS to the source
        arguments (aliased), so element writes propagate to the caller. */
    bool aliased = stackBuffer && borrow && !BuilderIsOwningType(b, elementType);
    LLVMTypeRef slotTy = aliased ? b->m_ptrTy : elemTy;

    size_t count = elements->count;
    LLVMValueRef dataPtr = LLVMConstNull(b->m_ptrTy);

    if (count > 0)
    {
        if (stackBuffer)
        {
            /* Fixed-size stack buffer (count is a compile-time constant). */
            size_t n = count;
            LLVMTypeRef arrTy = LLVMArrayType(slotTy, (unsigned)n);
            LLVMValueRef bufSlot = EntryAlloca(b, arrTy, "varargs");

            LLVMValueRef zero = LLVMConstInt(I64Ty(b), 0, 0);
            LLVMValueRef idx[2] = {zero, zero};
            dataPtr = LLVMBuildGEP2(b->m_builder, arrTy, bufSlot, idx, 2, "varargsdata");
        }
        else
        {
            LLVMValueRef elemSize = SizeOfConst(b, elemTy);
            LLVMValueRef countConst = LLVMConstInt(I64Ty(b), (unsigned long long)count, 0);
            LLVMValueRef totalBytes = LLVMBuildMul(b->m_builder, elemSize, countConst, "bytes");
            LLVMValueRef allocArgs[1] = {totalBytes};
            StrataAllocFn(b);
            dataPtr = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, allocArgs, 1, "arrbuf");
        }

        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);

        for (size_t i = 0; i < count; i++)
        {
            Node* eNode = (Node*)VecGet(elements, i);
            Value v = EmitExpr(b, eNode);

            LLVMValueRef idxArg[1] = {LLVMConstInt(I64Ty(b), (unsigned long long)i, 0)};
            LLVMValueRef elemAddr = LLVMBuildGEP2(b->m_builder, slotTy, dataPtr, idxArg, 1, "ael");

            if (aliased)
            {
                /* Store a POINTER to the source argument. A box arg already
                   is the value's address; lvalues yield their address; a
                   non-lvalue is materialized into a stack temp. */
                if (v.typeDesc.isBox)
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                }
                else
                {
                    LValue lv = EmitLValue(b, eNode);

                    if (lv.valid)
                    {
                        LLVMBuildStore(b->m_builder, lv.ptr, elemAddr);
                    }
                    else
                    {
                        LLVMValueRef temp = EntryAlloca(b, elemTy, "aliastemp");
                        LLVMBuildStore(b->m_builder, Coerce(b, v, elemTd).value, temp);
                        LLVMBuildStore(b->m_builder, temp, elemAddr);
                    }
                }

                continue;
            }

            if (elemTd.isString)
            {
                /* string element: literals are heap-copied (the constant is
                   borrowed; the array owns its elements); variables move. */
                if (borrow)
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                }
                else if (eNode->kind == NodeStrLiteral)
                {
                    LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, v.value, 0, "aelit.p");
                    LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, v.value, 1, "aelit.l");
                    LLVMBuildStore(b->m_builder, BuildOwnedStringFat(b, sp, sl), elemAddr);
                }
                else
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                    NullMovedSource(b, eNode);
                }
            }
            else if (elemTd.isArray)
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
                /* ^T element. */
                bool sameOwningType = v.typeDesc.isBox && v.typeDesc.boxInner
                                      && strcmp(v.typeDesc.boxInner->name, elemTd.boxInner->name) == 0;

                if (borrow)
                {
                    /* ref rest: borrow, keep source alive. */
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                }
                else if (sameOwningType)
                {
                    /* Same ^T type: move. */
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                    NullMovedSource(b, eNode);
                }
                else
                {
                    /* ^T from a non-^T value: box it up. */
                    LLVMValueRef heap = EmitBoxCell(b, elemTd.boxInner, v, eNode, "abox");
                    LLVMBuildStore(b->m_builder, heap, elemAddr);
                }
            }
            else if (elemTd.isBox)
            {
                /* string element. */
                if (borrow)
                {
                    LLVMBuildStore(b->m_builder, v.value, elemAddr);
                }
                else
                {
                    LLVMValueRef owned = EmitOwnedValue(b, v, eNode, StringTypeName(b));
                    LLVMBuildStore(b->m_builder, owned, elemAddr);
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
    td.arrayInner = elementType;
    td.aliasedArray = aliased;

    return ValueMake(arr, td);
}

static Value EmitArrayInit(Builder* b, ArrayInitExpr* n)
{
    /* An untyped non-empty literal means no context ever supplied an
        element type (a gap in inference) - diagnose instead of crashing
        on a NULL type. */
    if (!n->elementType && n->elements.count > 0)
    {
        if (b->m_diag)
        {
            DiagError(b->m_diag, n->base.range, "array literal element type could not be inferred");
        }

        n->elements.count = 0;
    }

    /* An empty braced literal with no contextual element type (`FooBar("x", {})`)
        is the canonical empty array - nothing to resolve or allocate. */
    if (!n->elementType && n->elements.count == 0)
    {
        LLVMValueRef arr = LLVMGetUndef(ArrayStructType(b));
        arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstNull(b->m_ptrTy), 0, "arri");
        arr = LLVMBuildInsertValue(b->m_builder, arr, LLVMConstInt(I64Ty(b), 0, 0), 1, "arrl");

        TypeDesc td = {0};
        td.type = ArrayStructType(b);
        td.isArray = true;
        return ValueMake(arr, td);
    }

    return EmitArrayFromNodes(b, n->elementType, &n->elements, false, false);
}

/* Product of the fixed dimensions at/below `t` (1 once a non-fixed node is
   reached). Used to map a flat literal index onto nested array dimensions. */
static long FixedDimProduct(const TypeName* t)
{
    long count = 1;

    while (t && t->isArray && t->length >= 0)
    {
        count *= t->length;
        t = t->elem;
    }

    return count;
}

/* Inserts element `k` (a FLAT index, C-initialized-array style) of a
   (possibly nested) fixed array value, descending through inner fixed
   dimensions with extract/insert pairs. */
static LLVMValueRef InsertFixedElem(Builder* b, LLVMValueRef agg, LLVMValueRef val, const TypeName* arrType, long k)
{
    if (!(arrType->isArray && arrType->length >= 0))
    {
        return agg;
    }

    if (arrType->elem && arrType->elem->isArray && arrType->elem->length >= 0)
    {
        long innerCount = FixedDimProduct(arrType->elem);
        long outer = innerCount > 0 ? k / innerCount : 0;
        long inner = innerCount > 0 ? k % innerCount : 0;

        LLVMValueRef sub = LLVMBuildExtractValue(b->m_builder, agg, (unsigned)outer, "sub");
        sub = InsertFixedElem(b, sub, val, arrType->elem, inner);
        return LLVMBuildInsertValue(b->m_builder, agg, sub, (unsigned)outer, "ins");
    }

    return LLVMBuildInsertValue(b->m_builder, agg, val, (unsigned)k, "ins");
}

static Value EmitStructInit(Builder* b, StructInitExpr* n)
{
    TypeName tn = MakeTypeName(b, n->typeName);
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

        if (fieldTd.isFixedArray && field->value->kind == NodeArrayInit)
        {
            /* Fixed-size array field from a braced literal: the flat list
                carries row-major elements (sema placed nested rows at their
                offsets); NULL entries are holes that stay zero. */
            ArrayInitExpr* ai = (ArrayInitExpr*)field->value;
            TypeDesc elemTd = Resolve(b, ai->elementType);

            LLVMValueRef arr = LLVMConstNull(fieldTd.type);

            for (size_t k = 0; k < ai->elements.count; k++)
            {
                Node* elem = (Node*)VecGet(&ai->elements, k);

                if (!elem)
                {
                    continue;
                }

                Value ev = Coerce(b, EmitExpr(b, elem), elemTd);
                arr = InsertFixedElem(b, arr, ev.value, &fieldDecl->type, (long)k);
            }

            agg = LLVMBuildInsertValue(b->m_builder, agg, arr, PhysicalFieldIndex(st, (int)idx), "ins");
            continue;
        }

        Value rawField = EmitExpr(b, field->value);
        Value fieldValue;

        if (fieldTd.isString && field->value->kind == NodeStrLiteral)
        {
            /* string field from a literal -> heap copy (safe to free). */
            LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, rawField.value, 0, "fslit.p");
            LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, rawField.value, 1, "fslit.l");
            fieldValue = ValueMake(BuildOwnedStringFat(b, sp, sl), fieldTd);
        }
        else if (fieldTd.isBox && fieldTd.boxInner
                 && !(rawField.typeDesc.isBox && rawField.typeDesc.boxInner
                      && strcmp(rawField.typeDesc.boxInner->name, fieldTd.boxInner->name) == 0))
        {
            /* ^T field from a non-^T value (bare T, string literal,
               string variable into ^string): allocate a T slot, construct
               the owned inner value, store it. Same pattern as a top-level
               ^T init. */
            LLVMValueRef heap = EmitBoxCell(b, fieldTd.boxInner, rawField, field->value, "fieldbox");
            fieldValue = ValueMake(heap, fieldTd);
        }
        else
        {
            fieldValue = Coerce(b, rawField, fieldTd);
        }

        agg = LLVMBuildInsertValue(b->m_builder, agg, fieldValue.value, PhysicalFieldIndex(st, (int)idx), "ins");

        /* If an owning field was moved from an owning lvalue source (string,
           ^T, or a dynamic array), null the source so its scope-exit drop
           is a no-op. */
        if (((fieldTd.isBox && (rawField.typeDesc.isBox || rawField.typeDesc.isString))
             || (fieldTd.isArray && rawField.typeDesc.isArray))
            && field->value->kind != NodeStrLiteral && field->value->kind != NodeArrayInit)
        {
            NullMovedSource(b, field->value);
        }
    }

    return ValueMake(agg, typeDesc);
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
        if (literal->value > 0x7FFFFFFFULL)
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

        size_t len = strlen(literal->value);
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

        /* A literal is a borrowed fat {ptr, len}: no allocation, NUL at
           [len]. Owning consumers heap-copy via EmitOwnedValue. */
        TypeDesc td = StringFatDesc(b);
        LLVMValueRef fat = LLVMConstNamedStruct(
            ArrayStructType(b), (LLVMValueRef[2]){gep, LLVMConstInt(I64Ty(b), (unsigned long long)len, 0)}, 2);

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

    case NodeNullTest:
    {
        /* `expr?` - test whether the optional holds anything (zero cost
           beyond the compare; sema guarantees a nullable path). A T?/^T slot
           tests the pointer; an optional array tests its fat struct: set =
           non-null data OR non-zero length. */
        NullTestExpr* nt = (NullTestExpr*)n;
        LValue lv = EmitLValue(b, nt->operand);

        if (lv.valid && lv.typeDesc.isArray)
        {
            LLVMValueRef data = LLVMBuildLoad2(b->m_builder, b->m_ptrTy, lv.ptr, "oad");
            LLVMValueRef len = LLVMBuildLoad2(b->m_builder, I64Ty(b), ArrayLenPtr(b, lv.ptr), "oal");

            LLVMValueRef dataNonNull = LLVMBuildICmp(b->m_builder, LLVMIntNE, data, LLVMConstNull(b->m_ptrTy), "nn");
            LLVMValueRef lenNonZero = LLVMBuildICmp(b->m_builder, LLVMIntNE, len, LLVMConstInt(I64Ty(b), 0, 0), "nz");

            return ValueMake(LLVMBuildOr(b->m_builder, dataNonNull, lenNonZero, "opt"), TypeDescMake(I1Ty(b), 0, NULL));
        }

        LLVMValueRef slotPtr = lv.valid ? lv.ptr : EmitExpr(b, nt->operand).value;
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

        LLVMValueRef valPtr = lv.ptr;
        TypeDesc valTd = lv.typeDesc;

        if (lv.typeDesc.isBox)
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
        Value operand = EmitExpr(b, cast->operand);
        TypeDesc target = Resolve(b, &cast->type);

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

        if ((operand.typeDesc.isBox && target.isBox) || (operand.typeDesc.isString && target.isString))
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
                /* Returns ^T but expr is a plain T: box it, like a local. */
                TypeDesc innerTd = Resolve(b, b->m_curRet.boxInner);
                LLVMValueRef size = SizeOfConst(b, innerTd.type);
                LLVMValueRef args[1] = {size};
                StrataAllocFn(b);
                LLVMValueRef heap = LLVMBuildCall2(b->m_builder, b->m_allocFnType, b->m_allocFn, args, 1, "heap");
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

                Node* movedReturnNode
                    = (v.typeDesc.isBox || v.typeDesc.isArray) ? (Node*)MovableBoxSourceNode(r->value) : NULL;

                if (movedReturnNode)
                {
                    LValue src = EmitLValueForNullStore(b, movedReturnNode);

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
                else if (typeDesc.isString && varDecl->init->kind == NodeStrLiteral)
                {
                    /* string from a literal: heap-copy (the constant is
                       borrowed; the binding owns its bytes). */
                    Value value = EmitExpr(b, varDecl->init);
                    LLVMValueRef sp = LLVMBuildExtractValue(b->m_builder, value.value, 0, "vslit.p");
                    LLVMValueRef sl = LLVMBuildExtractValue(b->m_builder, value.value, 1, "vslit.l");
                    LLVMValueRef owned = BuildOwnedStringFat(b, sp, sl);
                    LLVMBuildStore(b->m_builder, owned, slot);
                }
                else
                {
                    /* Move from another array/string binding: copy the
                       {ptr,len} struct and zero the source (whole slot) to
                       avoid a double free. */
                    Node* movedNode = (Node*)MovableBoxSourceNode(varDecl->init);
                    LValue src = movedNode ? EmitLValueForNullStore(b, movedNode) : (LValue){0};

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
                bool isLiteral = varDecl->init->kind == NodeStrLiteral;

                /* Direct move: source and destination are the same owning type
                   (string=string, ^T=^T). Copy the pointer, null the
                   source. No allocation needed. */
                bool sameOwningType = value.typeDesc.isBox && !isLiteral
                                      && ((typeDesc.boxInner == NULL && value.typeDesc.boxInner == NULL)
                                          || (typeDesc.boxInner && value.typeDesc.boxInner
                                              && strcmp(typeDesc.boxInner->name, value.typeDesc.boxInner->name) == 0));

                if (sameOwningType)
                {
                    LLVMBuildStore(b->m_builder, value.value, slot);
                    NullMovedSource(b, varDecl->init);
                }
                else if (typeDesc.boxInner)
                {
                    /* ^T: allocate a T slot, construct the owned inner into it,
                       store. For ^string this is: construct a string (heap-copy
                       literal / move source), then box it up - identical to ^int
                       but the inner happens to be owning. */
                    LLVMValueRef heap = EmitBoxCell(b, typeDesc.boxInner, value, varDecl->init, "heap");
                    LLVMBuildStore(b->m_builder, heap, slot);
                }
                else
                {
                    /* string (owning primitive, no box inner): construct the
                        owned value directly into the slot. */
                    LLVMValueRef owned = EmitOwnedValue(b, value, varDecl->init, StringTypeName(b));
                    LLVMBuildStore(b->m_builder, owned, slot);
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

    case NodeDefer:
    {
        DeferStmt* d = (DeferStmt*)n;
        BlockScope* scope = TopScope(b);

        if (scope)
        {
            VecPush(&scope->defers, d->stmt);
        }

        return;
    }

    case NodeBlock:
    {
        Block* blk = (Block*)n;
        size_t mark = b->m_owningLocals.count;

        PushScope(b);

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

        PopScope(b);

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
        PushScope(b);
        EmitStmt(b, i->thenBranch);
        if (!b->m_terminated)
        {
            RunDefers(b, TopScope(b));
        }
        PopScope(b);
        Br(b, endBB);

        if (i->elseBranch)
        {
            PositionAtEnd(b, elseBB);
            PushScope(b);
            EmitStmt(b, i->elseBranch);
            if (!b->m_terminated)
            {
                RunDefers(b, TopScope(b));
            }
            PopScope(b);
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
        loop->headerMark = b->m_owningLocals.count;
        loop->bodyMark = b->m_owningLocals.count;
        loop->scopeDepth = b->m_scopes.count;
        VecPush(&b->m_loops, loop);

        PushScope(b);
        EmitStmt(b, w->body);
        bool term = b->m_terminated;
        if (!term)
        {
            RunDefers(b, TopScope(b));
        }
        PopScope(b);
        VecPop(&b->m_loops);

        if (!term)
        {
            Br(b, condBB);
        }

        PositionAtEnd(b, endBB);

        return;
    }

    case NodeFor:
    {
        ForStmt* fs = (ForStmt*)n;

        size_t headerMark = b->m_owningLocals.count;

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
        loop->headerMark = headerMark;
        loop->bodyMark = b->m_owningLocals.count;
        loop->scopeDepth = b->m_scopes.count;
        VecPush(&b->m_loops, loop);

        PushScope(b);
        EmitStmt(b, fs->body);
        bool term = b->m_terminated;
        if (!term)
        {
            RunDefers(b, TopScope(b));
        }
        PopScope(b);
        VecPop(&b->m_loops);

        if (!term)
        {
            Br(b, updBB);
        }

        PositionAtEnd(b, updBB);

        if (fs->update)
        {
            (void)EmitExpr(b, fs->update);
        }

        if (!term)
        {
            Br(b, condBB);
        }

        PositionAtEnd(b, endBB);

        return;
    }

    case NodeBreak:
    {
        if (b->m_loops.count > 0)
        {
            Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
            RunDefersFrom(b, loop->scopeDepth);
            EmitDrops(b, loop->headerMark);
            b->m_owningLocals.count = loop->headerMark;
            Br(b, loop->end);
        }
        else
        {
            DiagError(b->m_diag, n->range, "break statement not inside a loop");
        }

        return;
    }

    case NodeContinue:
    {
        if (b->m_loops.count > 0)
        {
            Loop* loop = (Loop*)VecGet(&b->m_loops, b->m_loops.count - 1);
            RunDefersFrom(b, loop->scopeDepth);
            EmitDrops(b, loop->bodyMark);
            b->m_owningLocals.count = loop->bodyMark;
            Br(b, loop->cont);
        }
        else
        {
            DiagError(b->m_diag, n->range, "continue statement not inside a loop");
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

/* Convert a folded constant to `td` (const-level mirror of Coerce). Integer
 * width truncation is left to LLVMConstInt (same as direct literal lowering). */
static ConstInitVal CastConstVal(Builder* b, ConstInitVal v, TypeDesc td)
{
    if (td.type == I1Ty(b)) /* conversion to bool is a (non)zero test */
    {
        ConstInitVal r;
        r.kind = CIK_BOOL;
        r.i = (v.kind == CIK_FLOAT ? v.f != 0.0 : v.i != 0) ? 1 : 0;
        r.f = 0.0;
        return r;
    }

    if (td.isFloat)
    {
        if (v.kind == CIK_FLOAT)
        {
            return v;
        }

        ConstInitVal r;
        r.kind = CIK_FLOAT;
        r.i = 0;
        r.f = (double)(long long)v.i;
        return r;
    }

    if (v.kind == CIK_FLOAT)
    {
        ConstInitVal r;
        r.kind = CIK_INT;
        r.i = (unsigned long long)(long long)v.f;
        r.f = 0.0;
        return r;
    }

    return v;
}

/* Integer constant arithmetic in two's complement; `div`/`mod`/`>>` apply
 * C signed (long long) semantics; refuses division/modulo by zero and
 * shifts whose count is negative or >= 64. */
static bool FoldConstIntArith(BinaryOp op, unsigned long long a, unsigned long long b, unsigned long long* out)
{
    switch (op)
    {
    case BinAdd:
        *out = a + b;
        return true;
    case BinSub:
        *out = a - b;
        return true;
    case BinMul:
        *out = a * b;
        return true;
    case BinDiv:
        if (b == 0)
        {
            return false;
        }

        *out = (unsigned long long)((long long)a / (long long)b);
        return true;
    case BinMod:
        if (b == 0)
        {
            return false;
        }

        *out = (unsigned long long)((long long)a % (long long)b);
        return true;
    case BinBitAnd:
        *out = a & b;
        return true;
    case BinBitOr:
        *out = a | b;
        return true;
    case BinBitXor:
        *out = a ^ b;
        return true;
    case BinShl:
        if (b >= 64)
        {
            return false;
        }

        *out = a << b;
        return true;
    case BinShr:
        if (b >= 64)
        {
            return false;
        }

        *out = (unsigned long long)((long long)a >> b);
        return true;
    default:
        return false;
    }
}

/* Float constant arithmetic (`+ - * /`); refuses division by zero so the
 * fold never embeds an inf/nan literal. */
static bool FoldConstFloatArith(BinaryOp op, double a, double b, double* out)
{
    switch (op)
    {
    case BinAdd:
        *out = a + b;
        return true;
    case BinSub:
        *out = a - b;
        return true;
    case BinMul:
        *out = a * b;
        return true;
    case BinDiv:
        if (b == 0.0)
        {
            return false;
        }

        *out = a / b;
        return true;
    default:
        return false;
    }
}

static bool FoldConstCompare(BinaryOp op, long long a, long long b, bool* res)
{
    switch (op)
    {
    case BinEqEq:
        *res = a == b;
        return true;
    case BinNotEq:
        *res = a != b;
        return true;
    case BinLt:
        *res = a < b;
        return true;
    case BinLtEq:
        *res = a <= b;
        return true;
    case BinGt:
        *res = a > b;
        return true;
    case BinGtEq:
        *res = a >= b;
        return true;
    default:
        return false;
    }
}

static bool FoldConstCompareFloat(BinaryOp op, double a, double b, bool* res)
{
    switch (op)
    {
    case BinEqEq:
        *res = a == b;
        return true;
    case BinNotEq:
        *res = a != b;
        return true;
    case BinLt:
        *res = a < b;
        return true;
    case BinLtEq:
        *res = a <= b;
        return true;
    case BinGt:
        *res = a > b;
        return true;
    case BinGtEq:
        *res = a >= b;
        return true;
    default:
        return false;
    }
}

/* Constant-fold a global initializer expression (literals, unary/binary
 * operators with C constant-expression semantics, scalar casts, and
 * references to other manifest constants). Values are folded at their
 * NATURAL kind (raw int/float) — conversion to the declared global type
 * happens once at the init site (and inside explicit NodeCasts), so a
 * `bool` target must never collapse operands before an operator applies.
 * Returns false when the expression is not a compile-time constant. */
static bool FoldConstInit(Builder* b, TypeDesc td, Node* n, ConstInitVal* out)
{
    switch (n->kind)
    {
    case NodeIntLiteral:
        *out = (ConstInitVal){CIK_INT, .i = ((IntLiteral*)n)->value, .f = 0.0};
        return true;
    case NodeFloatLiteral:
        *out = (ConstInitVal){CIK_FLOAT, .i = 0, .f = ((FloatLiteral*)n)->value};
        return true;
    case NodeBoolLiteral:
        *out = (ConstInitVal){CIK_BOOL, .i = ((BoolLiteral*)n)->value ? 1 : 0, .f = 0.0};
        return true;
    case NodeUnary:
    {
        UnaryExpr* u = AsNode(UnaryExpr, n);
        ConstInitVal inner;

        if (!FoldConstInit(b, td, u->operand, &inner))
        {
            return false;
        }

        switch (u->op)
        {
        case UnNeg:
            if (inner.kind == CIK_FLOAT)
            {
                inner.f = -inner.f;
            }
            else
            {
                inner.i = 0ULL - inner.i;
            }

            break;
        case UnPos:
            break;
        case UnNot:
        {
            bool isZero = (inner.kind == CIK_FLOAT) ? inner.f == 0.0 : inner.i == 0;

            inner.kind = CIK_INT;
            inner.i = isZero;
            inner.f = 0.0;
            break;
        }
        case UnBitNot:
            if (inner.kind == CIK_FLOAT)
            {
                return false;
            }

            inner.kind = CIK_INT;
            inner.i = ~inner.i;
            break;
        default:
            return false;
        }

        *out = inner;
        return true;
    }
    case NodeBinary:
    {
        BinaryExpr* e = AsNode(BinaryExpr, n);
        ConstInitVal l;
        ConstInitVal r;

        if (!FoldConstInit(b, td, e->lhs, &l) || !FoldConstInit(b, td, e->rhs, &r))
        {
            return false;
        }

        bool lFloat = l.kind == CIK_FLOAT;
        bool rFloat = r.kind == CIK_FLOAT;

        switch (e->op)
        {
        case BinAdd:
        case BinSub:
        case BinMul:
        case BinDiv:
        {
            if (lFloat || rFloat)
            {
                double a = lFloat ? l.f : (double)(long long)l.i;
                double z = rFloat ? r.f : (double)(long long)r.i;

                out->kind = CIK_FLOAT;
                out->i = 0;
                return FoldConstFloatArith(e->op, a, z, &out->f);
            }

            out->kind = CIK_INT;
            out->f = 0.0;
            return FoldConstIntArith(e->op, l.i, r.i, &out->i);
        }
        case BinMod:
        case BinBitAnd:
        case BinBitOr:
        case BinBitXor:
        case BinShl:
        case BinShr:
        {
            if (lFloat || rFloat)
            {
                return false;
            }

            out->kind = CIK_INT;
            out->f = 0.0;
            return FoldConstIntArith(e->op, l.i, r.i, &out->i);
        }
        case BinEqEq:
        case BinNotEq:
        case BinLt:
        case BinLtEq:
        case BinGt:
        case BinGtEq:
        {
            bool res;

            if (lFloat || rFloat)
            {
                double a = lFloat ? l.f : (double)(long long)l.i;
                double z = rFloat ? r.f : (double)(long long)r.i;

                if (!FoldConstCompareFloat(e->op, a, z, &res))
                {
                    return false;
                }
            }
            else if (!FoldConstCompare(e->op, (long long)l.i, (long long)r.i, &res))
            {
                return false;
            }

            out->kind = CIK_INT;
            out->i = res ? 1 : 0;
            out->f = 0.0;
            return true;
        }
        case BinLogicAnd:
        case BinLogicOr:
        {
            bool a = lFloat ? l.f != 0.0 : l.i != 0;
            bool z = rFloat ? r.f != 0.0 : r.i != 0;

            out->kind = CIK_INT;
            out->i = (e->op == BinLogicAnd) ? (a && z) : (a || z);
            out->f = 0.0;
            return true;
        }
        default:
            return false;
        }
    }
    case NodeCast:
    {
        CastExpr* c = AsNode(CastExpr, n);
        TypeDesc innerTd = Resolve(b, &c->type);

        if (innerTd.isVoid || innerTd.structTypeName || innerTd.isArray || innerTd.isBox || innerTd.isSimdVector)
        {
            return false;
        }

        ConstInitVal inner;

        if (!FoldConstInit(b, innerTd, c->operand, &inner))
        {
            return false;
        }

        /* First to the cast's own target type (the truncation/narrowing the
           user asked for, e.g. `(int)1.5`), then to the declared global
           type. */
        *out = CastConstVal(b, CastConstVal(b, inner, innerTd), td);
        return true;
    }
    case NodeIdent:
    {
        /* Chained manifest constants: `const B = A;` */
        ConstValueSlot* cv = (ConstValueSlot*)StrMapGet(&b->m_constValues, ((IdentExpr*)n)->name);

        if (!cv)
        {
            return false;
        }

        *out = cv->civ;
        return true;
    }
    default:
        return false;
    }
}

static BuiltModule BuilderBuild(Builder* b, const Module* module, DiagnosticEngine* diag, bool jitMode,
                                const StrataProfile* profile)
{
    b->m_diag = diag;
    b->m_jitMode = jitMode;
    b->m_boundsCheck = !profile || profile->boundsCheck;
    b->m_nullExternCheck = !profile || profile->nullExternCall;
    b->m_ctx = LLVMContextCreate();
    b->m_mod = LLVMModuleCreateWithNameInContext(module->name, b->m_ctx);
    b->m_builder = LLVMCreateBuilderInContext(b->m_ctx);
    b->m_ptrTy = LLVMPointerTypeInContext(b->m_ctx, 0);
    b->m_strLitCount = 0;

    TypeRegistryBuild(&b->m_registry, module);

    /* Impl property table: "Handle.Prop" -> accessors, resolved against the
       module's function list (sema guarantees the externs exist). */
    for (size_t i = 0; i < module->impls.count; i++)
    {
        const ImplDecl* impl = (const ImplDecl*)VecGet((Vec*)&module->impls, i);

        for (size_t j = 0; j < impl->properties.count; j++)
        {
            PropertyDecl* prop = (PropertyDecl*)VecGet(&impl->properties, j);
            ImplPropEntry* entry = (ImplPropEntry*)arena_alloc(b->m_arena, sizeof(ImplPropEntry));
            entry->decl = prop;
            entry->getter = FindModuleFunction(module, prop->getterSymbol);
            entry->setter = FindModuleFunction(module, prop->setterSymbol);

            StrMapPut(&b->m_implProps, arena_format(b->m_arena, "%s.%s", impl->handleName, prop->name), entry);
        }
    }

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

        /* Build the physical member list: registry-computed [n x i8] pads
           interleaved before the fields they follow. A struct with any
           explicit fieldoffset is emitted packed (pad members fully determine
           layout); natural structs (padCount 0) produce the same body. */
        size_t memberCount = st->hasLayout ? st->physicalCount : st->fields.count;
        LLVMTypeRef* members = NULL;

        if (memberCount > 0)
        {
            members = (LLVMTypeRef*)arena_alloc(b->m_arena, memberCount * sizeof(LLVMTypeRef));
        }

        LLVMTypeRef i8Ty = LLVMInt8TypeInContext(b->m_ctx);
        size_t nextPad = 0;
        unsigned m = 0;

        for (size_t j = 0; j < st->fields.count; j++)
        {
            while (st->hasLayout && nextPad < st->padCount && st->pads[nextPad].beforeField == j)
            {
                members[m++] = LLVMArrayType(i8Ty, (unsigned)st->pads[nextPad].bytes);
                nextPad++;
            }

            FieldDecl* fieldDecl = (FieldDecl*)VecGet(&st->fields, j);
            members[m++] = Resolve(b, &fieldDecl->type).type;
        }

        while (st->hasLayout && nextPad < st->padCount)
        {
            members[m++] = LLVMArrayType(i8Ty, (unsigned)st->pads[nextPad].bytes);
            nextPad++;
        }

        LLVMTypeRef structTy = (LLVMTypeRef)StrMapGet(&b->m_structTypes, st->name);
        LLVMStructSetBody(structTy, members, (unsigned)memberCount, st->packedLayout ? 1 : 0);
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

        /* ^T / T[] / string / alias-of-string globals: storage starts null
           (box) or zero (array). The runtime init (__strata_module_init)
           fills them and __strata_module_teardown drops them. */
        if (BuilderIsOwningType(b, &gd->type))
        {
            LLVMValueRef init = LLVMConstNull(typeDesc.type);
            LLVMValueRef global = LLVMAddGlobal(b->m_mod, typeDesc.type, gd->name);
            LLVMSetInitializer(global, init);

            Value* sym = (Value*)arena_alloc(b->m_arena, sizeof(Value));
            sym->value = global;
            sym->typeDesc = typeDesc;
            StrMapPut(&b->m_globals, gd->name, sym);
            continue;
        }

        LLVMValueRef init = LLVMConstNull(typeDesc.type);
        ConstInitVal foldedVal;
        bool folded = false;

        if (gd->init)
        {
            folded = FoldConstInit(b, typeDesc, gd->init, &foldedVal);

            if (folded)
            {
                /* The fold is type-agnostic (natural int/float kinds);
                   convert to the declared global type once here. */
                foldedVal = CastConstVal(b, foldedVal, typeDesc);
                init = typeDesc.isFloat ? LLVMConstReal(typeDesc.type, foldedVal.f)
                                        : LLVMConstInt(typeDesc.type, foldedVal.i, 0);
            }
            else if (b->m_diag)
            {
                DiagErrorFmt(b->m_diag, gd->base.range,
                             "global '%s' initializer must be a compile-time constant "
                             "(scalar literals and operator expressions over them "
                             "and other const globals)",
                             gd->name);
            }
        }

        /* A `const` scalar global with a constant initializer is a MANIFEST
           constant: no storage, no symbol — uses inline the value. This is
           the C++-safe alternative to `const int X = N;` globals (C++ gives
           const globals internal linkage, so the symbol can never be linked
           against) and the enabler for `[constName]` array dimensions. */
        if (gd->type.isConst && folded)
        {
            ConstValueSlot* cs = (ConstValueSlot*)arena_alloc(b->m_arena, sizeof(ConstValueSlot));
            cs->civ = foldedVal;
            cs->td = typeDesc;
            cs->constant = init;
            StrMapPut(&b->m_constValues, gd->name, cs);
            continue;
        }

        LLVMValueRef global = LLVMAddGlobal(b->m_mod, typeDesc.type, gd->name);
        LLVMSetInitializer(global, init);

        if (gd->type.isConst)
        {
            LLVMSetGlobalConstant(global, 1);
        }

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

    /* Emit __strata_module_init + __strata_module_teardown when the module
       has owning globals (^T / T[] / string / alias) that need runtime
       initialization or teardown. */
    {
        bool hasOwningGlobal = false;

        for (size_t i = 0; i < module->globals.count; i++)
        {
            GlobalDecl* gd = (GlobalDecl*)VecGet(&module->globals, i);

            if (BuilderIsOwningType(b, &gd->type))
            {
                hasOwningGlobal = true;
                break;
            }
        }

        if (hasOwningGlobal)
        {
            LLVMTypeRef voidTy = LLVMVoidTypeInContext(b->m_ctx);
            LLVMTypeRef initTy = LLVMFunctionType(voidTy, NULL, 0, 0);

            /* ---- __strata_module_init ---- */
            LLVMValueRef initFn = LLVMAddFunction(b->m_mod, "__strata_module_init", initTy);
            b->m_curFn = initFn;
            StrMapClear(&b->m_symbols);
            b->m_terminated = false;
            b->m_loops.count = 0;
            b->m_owningLocals.count = 0;
            LLVMBasicBlockRef initEntry = LLVMAppendBasicBlockInContext(b->m_ctx, initFn, "entry");
            b->m_entryBlock = initEntry;
            b->m_entryAllocaPt = NULL;
            LLVMPositionBuilderAtEnd(b->m_builder, initEntry);

            for (size_t i = 0; i < module->globals.count; i++)
            {
                GlobalDecl* gd = (GlobalDecl*)VecGet(&module->globals, i);

                if (!BuilderIsOwningType(b, &gd->type))
                {
                    continue;
                }

                if (!gd->init)
                {
                    continue;
                }

                Value* sym = (Value*)StrMapGet(&b->m_globals, gd->name);

                if (!sym)
                {
                    continue;
                }

                TypeDesc td = sym->typeDesc;

                if (td.isArray && gd->init->kind == NodeArrayInit)
                {
                    LLVMValueRef arr = EmitArrayInit(b, AsNode(ArrayInitExpr, gd->init)).value;
                    LLVMBuildStore(b->m_builder, arr, sym->value);
                }
                else if (td.isString)
                {
                    /* string / alias-of-string global: construct the owned
                       value directly into the slot (heap-copy a literal; take
                       a call result). Teardown drops it. */
                    Value val = EmitExpr(b, gd->init);
                    LLVMValueRef owned = EmitOwnedValue(b, val, gd->init, StringTypeName(b));
                    LLVMBuildStore(b->m_builder, owned, sym->value);
                }
                else if (td.isBox && td.boxInner)
                {
                    Value val = EmitExpr(b, gd->init);

                    /* Direct move from the same ^T kind: take pointer. */
                    bool sameBoxKind
                        = val.typeDesc.boxInner && strcmp(val.typeDesc.boxInner->name, td.boxInner->name) == 0;

                    if (sameBoxKind)
                    {
                        LLVMBuildStore(b->m_builder, val.value, sym->value);
                    }
                    else
                    {
                        /* Box up the inner value (owning or not). */
                        LLVMValueRef heap = EmitBoxCell(b, td.boxInner, val, gd->init, "boxgl");
                        LLVMBuildStore(b->m_builder, heap, sym->value);
                    }
                }
            }

            LLVMBuildRetVoid(b->m_builder);

            /* ---- __strata_module_teardown ---- */
            LLVMValueRef tdFn = LLVMAddFunction(b->m_mod, "__strata_module_teardown", initTy);
            b->m_curFn = tdFn;
            StrMapClear(&b->m_symbols);
            b->m_terminated = false;
            b->m_loops.count = 0;
            b->m_owningLocals.count = 0;
            LLVMBasicBlockRef tdEntry = LLVMAppendBasicBlockInContext(b->m_ctx, tdFn, "entry");
            b->m_entryBlock = tdEntry;
            b->m_entryAllocaPt = NULL;
            LLVMPositionBuilderAtEnd(b->m_builder, tdEntry);

            for (size_t i = 0; i < module->globals.count; i++)
            {
                GlobalDecl* gd = (GlobalDecl*)VecGet(&module->globals, i);

                if (!BuilderIsOwningType(b, &gd->type))
                {
                    continue;
                }

                Value* sym = (Value*)StrMapGet(&b->m_globals, gd->name);

                if (!sym)
                {
                    continue;
                }

                EmitDropOne(b, sym->value, sym->typeDesc);
            }

            LLVMBuildRetVoid(b->m_builder);

            /* AOT: register the initializer as a CRT constructor
               (`llvm.global_ctors`, default priority) so it runs when a host
               links the object - no host-side call. ELF -> .init_array, COFF
               -> .CRT$XCU. JIT keeps its explicit call and skips this list. */
            if (!b->m_jitMode)
            {
                LLVMTypeRef fields[3] = {I32Ty(b), b->m_ptrTy, b->m_ptrTy};
                LLVMTypeRef ctorTy = LLVMStructTypeInContext(b->m_ctx, fields, 3, 0);
                LLVMTypeRef arrTy = LLVMArrayType(ctorTy, 1);
                LLVMValueRef elems[3] = {LLVMConstInt(I32Ty(b), 65535, 0), LLVMConstBitCast(initFn, b->m_ptrTy),
                                         LLVMConstNull(b->m_ptrTy)};
                LLVMValueRef entry = LLVMConstNamedStruct(ctorTy, elems, 3);
                LLVMValueRef arr = LLVMConstArray(ctorTy, &entry, 1);
                LLVMValueRef ctors = LLVMAddGlobal(b->m_mod, arrTy, "llvm.global_ctors");
                LLVMSetInitializer(ctors, arr);
                LLVMSetLinkage(ctors, LLVMAppendingLinkage);
            }
        }
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
    if (bm->mod)
    {
        LLVMDisposeModule(bm->mod);
        bm->mod = NULL;
    }
    if (bm->ctx)
    {
        LLVMContextDispose(bm->ctx);
        bm->ctx = NULL;
    }
    if (bm->externSymbols.items)
    {
        free(bm->externSymbols.items);
        bm->externSymbols.items = NULL;
        bm->externSymbols.count = 0;
        bm->externSymbols.cap = 0;
    }
}

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                            const StrataProfile* profile)
{
    Builder b = {0};
    b.m_arena = arena;
    StrMapInit(&b.m_structTypes);
    StrMapInit(&b.m_funcs);
    StrMapInit(&b.m_symbols);
    StrMapInit(&b.m_globals);
    StrMapInit(&b.m_constValues);
    StrMapInit(&b.m_externSlots);
    StrMapInit(&b.m_implProps);
    StrMapInit(&b.m_dropFns);
    VecInit(&b.m_externNames);
    VecInit(&b.m_loops);
    VecInit(&b.m_owningLocals);
    VecInit(&b.m_scopes);
    b.m_allocFn = NULL;
    b.m_allocFnType = NULL;
    b.m_freeFn = NULL;
    b.m_freeFnType = NULL;
    b.m_panicFn = NULL;
    b.m_panicFnType = NULL;
    b.m_oobFn = NULL;
    b.m_oobFnType = NULL;
    b.m_strdupFn = NULL;
    b.m_strdupFnType = NULL;
    b.m_arrayType = NULL;

    TypeRegistryInit(&b.m_registry);

    BuiltModule module = BuilderBuild(&b, ast, diag, jitMode, profile);
    StrMapFree(&b.m_structTypes);
    StrMapFree(&b.m_funcs);
    StrMapFree(&b.m_symbols);
    StrMapFree(&b.m_globals);
    StrMapFree(&b.m_constValues);
    StrMapFree(&b.m_externSlots);
    StrMapFree(&b.m_implProps);
    StrMapFree(&b.m_dropFns);
    free(b.m_loops.items);
    free(b.m_owningLocals.items);
    TypeRegistryFree(&b.m_registry);

    return module;
}
