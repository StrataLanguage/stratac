#pragma once

#include "AST/AST.h"
#include "Codegen/LLVMCApi.h"
#include "Codegen/LLVMModuleBuilder.h"
#include "Codegen/LLVMSimd.h"
#include "Codegen/TypeDescFlags.h"
#include "Codegen/TypeMetadata.h"
#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"
#include "Core/Diagnostics.h"
#include "Sema/ConstFold.h"
#include "strata/strata_types.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    LLVMValueRef function;
    LLVMTypeRef type;
    TypeDesc returnType;
    /* Pre-widening return type when WidenRetType applied: C hosts only
       define the low part of the register, so extern call results must be
       narrowed back before use. */
    LLVMTypeRef retSemanticTy;
    bool retSemanticUnsigned;
    bool externStringReturn; /* extern `string` (or alias) return: ABI is char*, copied into an owned fat */
    bool* paramByPtr;
    size_t paramByPtrCount;
    bool hasHiddenCtxParam; /* every non-extern fn: LLVM param 0 is the hidden context pointer, invisible
                                at the Strata source level (see DeclareFunction / EmitCall) */
    bool hasTypeDescParam;  /* `extern<T>` instantiation: a trailing `const StrataTypeDesc*` for T */
} FuncInfo;

// Folded constant init (host-side values; LLVM-C lacks Const*Cast).
typedef enum
{
    CIK_INT,
    CIK_FLOAT,
    CIK_BOOL
} ConstInitKind;

typedef struct
{
    ConstInitKind kind;
    unsigned long long i; // Int/bool payload.
    double f;             // Float payload.
    ConstIntType ity;     // Runtime integer type of `i` (see ConstFold.h); bits 0 = no narrowing.
} ConstInitVal;

// A `const` scalar global: no LLVM global emitted, uses inline the value.
typedef struct
{
    ConstInitVal civ;
    TypeDesc td;
    LLVMValueRef constant;
} ConstValueSlot;

typedef struct
{
    bool valid;
    bool vectorLane;        /* single-lane vector lvalue: ptr addresses the vector value */
    LLVMValueRef ptr;       /* memory holding the vector (field slot, array elem, box cell) */
    TypeDesc typeDesc;      /* lane scalar type when vectorLane (float) */
    LLVMTypeRef vectorType; /* LLVM vector type when vectorLane */
    unsigned lane;          /* lane index 0..3 when vectorLane */
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
    size_t bodyMark;   /* owning-locals count before the loop body (dropped on break/continue;
                          a `for` header's own locals are dropped at for.end) */
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

// -- Impl properties (methods are rewritten to plain extern calls by sema)
typedef struct
{
    PropertyDecl* decl;
    const FunctionDecl* getter; // NULL for write-only
    const FunctionDecl* setter; // NULL for read-only
} ImplPropEntry;

/* An owning piece (element / field) read out of a registered temporary,
   keyed by the AST node that read it. If that node is later moved into an
   owning destination, NullMovedSource nulls `addr` inside the temporary, so
   the temporary's end-of-statement drop frees everything except the moved
   piece. If it is only read, the drop frees it along with the rest. */
typedef struct TempPart
{
    const Node* node;
    LLVMValueRef addr;
    TypeDesc td;
    size_t tempIdx; /* index in m_temps of the owning temporary */
} TempPart;

typedef struct PseudoCallDefinition
{
    const char* name;
    Value (*callDef)(Builder* b, CallExpr* n);
} IntrinsicDefinition;

/* A caller-owned array-literal temp lent to a `ref T[]` param: the callee
   borrows it; the caller drops it right after the call returns. */
typedef struct
{
    LLVMValueRef slot;
    TypeDesc td;
    bool stackOnly; /* stack-constructed view: drop owning ELEMENTS, never free the buffer */
} OwnedArgTemp;

/* Prototypes for functions defined in one Builder/ file and used in another. */
/* IrUtil.c */
LLVMValueRef IcmpByName(LLVMBuilderRef builder, const char* pred, bool isUnsigned, LLVMValueRef l, LLVMValueRef r);
LLVMValueRef FcmpByName(LLVMBuilderRef builder, const char* pred, LLVMValueRef l, LLVMValueRef r);
LLVMTypeRef I32Ty(Builder* b);
LLVMTypeRef I64Ty(Builder* b);
LLVMTypeRef I1Ty(Builder* b);
LLVMTypeRef WidenRetType(Builder* b, LLVMTypeRef t);
LLVMValueRef ExtendToRetWidth(Builder* b, LLVMValueRef value, TypeDesc semantic, LLVMTypeRef retLlvm);
LLVMBasicBlockRef NewBb(Builder* b, const char* name);
LLVMValueRef EntryAlloca(Builder* b, LLVMTypeRef type, const char* name);
void PositionAtEnd(Builder* b, LLVMBasicBlockRef bb);
void Br(Builder* b, LLVMBasicBlockRef dest);
LLVMValueRef ConstIntTrunc(LLVMTypeRef ty, unsigned long long v);
LLVMValueRef IdxConst(Builder* b, unsigned i);
LLVMValueRef SizeOfConst(Builder* b, LLVMTypeRef ty);
LLVMValueRef EntryAllocaZeroed(Builder* b, LLVMTypeRef type, const char* name);

/* Types.c */
TypeDesc TypeDescMake(LLVMTypeRef type, TypeDescFlag flags, const char* structTypeName);
Value ValueMake(LLVMValueRef value, TypeDesc typeDesc);
TypeName MakeTypeName(Builder* b, const char* name);
TypeDesc ResolveByName(Builder* b, const char* name);
const TypeName* StringTypeName(Builder* b);
bool BuilderIsOwningType(Builder* b, const TypeName* t);
bool BuilderIsOwningValue(Builder* b, const TypeName* t);
bool TypeDescIsOwning(Builder* b, const TypeDesc* td);
TypeDesc Resolve(Builder* b, const TypeName* t);
LLVMValueRef ZeroOf(TypeDesc typeDesc);
LLVMValueRef DefaultValueOf(Builder* b, const TypeName* type, TypeDesc typeDesc);
Value ZeroInt(Builder* b);
Value Coerce(Builder* b, Value value, TypeDesc target);
LLVMValueRef TypeDescConstant(Builder* b, const char* structName);

/* Runtime.c */
const char* ExportedSymbolName(Builder* b, const char* name);
void MakeHelperModuleLocal(Builder* b, LLVMValueRef function);
LLVMValueRef StrataAllocFn(Builder* b);
LLVMValueRef AllocBoxCell(Builder* b, LLVMValueRef size, const char* name);
LLVMValueRef StrataFreeFn(Builder* b);
LLVMValueRef StrataPanicFn(Builder* b);
LLVMValueRef MsgGlobalPtr(Builder* b, const char* prefix, const char* message);
void EmitPanic(Builder* b, const char* message);
void EmitOobReport(Builder* b);
void EmitBoundsCheck(Builder* b, LLVMValueRef index, LLVMValueRef length);
void EmitPanicIf(Builder* b, LLVMValueRef bad, const char* message);

/* Strings.c */
LLVMValueRef EmitCStringLiteralPtr(Builder* b, const StrLiteral* literal);
TypeDesc StringFatDesc(Builder* b);
LLVMValueRef BuildOwnedStringFat(Builder* b, LLVMValueRef srcPtr, LLVMValueRef lenVal);
LLVMValueRef BuildConcatStringFat(Builder* b, LLVMValueRef aPtr, LLVMValueRef aLen, LLVMValueRef bPtr,
                                  LLVMValueRef bLen);
LLVMValueRef BuildStrLen(Builder* b, LLVMValueRef s);
LLVMValueRef BuildOwnedStringFromCStr(Builder* b, LLVMValueRef cstr);
LLVMValueRef EmptyNulString(Builder* b);
LLVMValueRef BuildExternStringPtr(Builder* b, Value v);
LLVMValueRef EmptyStrConstant(Builder* b);
LLVMValueRef StrataStrdupFn(Builder* b);
LLVMValueRef StrataStrEqFn(Builder* b);
LLVMValueRef StrataCStrLenFn(Builder* b);
Value EmitSubstringBuiltin(Builder* b, CallExpr* n);
Value EmitTextCast(Builder* b, CastExpr* cast);

/* Arrays.c */
LLVMTypeRef ArrayStructType(Builder* b);
LLVMValueRef ArrayDataPtr(Builder* b, LLVMValueRef slot);
LLVMValueRef ArrayLenPtr(Builder* b, LLVMValueRef slot);
LLVMValueRef ArrayCapPtr(Builder* b, LLVMValueRef slot);
LLVMValueRef WidenLen(Builder* b, LLVMValueRef v);
LLVMValueRef EmitCheckedElemPtr(Builder* b, LLVMValueRef idxVal, LLVMValueRef lenVal, LLVMValueRef dataPtr,
                                LLVMTypeRef elemTy, TypeDesc elemTd, bool aliased);
LLVMValueRef EmitCheckedFixedElemPtr(Builder* b, LLVMValueRef idxVal, long length, LLVMValueRef arrPtr,
                                     LLVMTypeRef arrTy, TypeDesc elemTd);
Value EmitArrayBuiltin(Builder* b, CallExpr* n);
Value EmitArrayValueCopy(Builder* b, Value src, TypeDesc td, TypeDesc elemTd, LLVMTypeRef elemTy, LLVMValueRef oldLen,
                         LLVMValueRef oldData);
Value EmitArrayFromNodes(Builder* b, const TypeName* elementType, const Vec* elements, bool stackBuffer, bool borrow);
Value EmitArrayInit(Builder* b, ArrayInitExpr* n);
long FixedDimProduct(const TypeName* t);
LLVMValueRef InsertFixedElem(Builder* b, LLVMValueRef agg, LLVMValueRef val, const TypeName* arrType, long k);

/* Boxes.c */
Value DerefBoxValue(Builder* b, Value value);
Value UnboxIfBox(Builder* b, Value value, TypeDesc target);
LValue EmitLValueForNullStore(Builder* b, Node* n);
void NullOwningSlot(Builder* b, LValue src);
LLVMValueRef EmitBoxCell(Builder* b, const TypeName* innerTn, Value src, Node* srcNode, const char* name);
LLVMValueRef EmitBoxCellCopy(Builder* b, Value src, TypeDesc td);
bool BoxIntoInlineOwning(Builder* b, TypeDesc targetTd, TypeDesc valueTd);
Value TakeBoxContents(Builder* b, Value box, Node* srcNode);

/* Structs.c */
unsigned PhysicalFieldIndex(const StructType* st, int idx);
LLVMValueRef InsertStructField(Builder* b, const StructType* st, LLVMValueRef agg, int idx, LLVMValueRef value);
LLVMValueRef ExtractStructField(Builder* b, const StructType* st, LLVMValueRef agg, int idx, LLVMTypeRef fieldTy);
LLVMValueRef GetOrCreateStructDropFn(Builder* b, const char* structName);
LLVMValueRef GlobalFieldPtr(Builder* b, const char* name);
Value EmitStructValueCopy(Builder* b, Value src, TypeDesc td);
LLVMValueRef GetOrCreateStructCopyFn(Builder* b, const char* structName);
bool FieldInitMovesSource(Builder* b, TypeDesc fieldTd, TypeDesc valueTd, const Node* valueNode);
Value EmitStructInit(Builder* b, StructInitExpr* n);
void MarkPackedFieldAccessesUnaligned(LLVMModuleRef mod);

/* Ownership.c */
LLVMValueRef SharedDummyAddr(Builder* b, TypeDesc td);
bool ElemNeedsScratchDummy(Builder* b, TypeDesc td);
void EmitDummyInit(Builder* b, LLVMValueRef slot, TypeDesc td);
void EmitDropOne(Builder* b, LLVMValueRef slot, TypeDesc td);
void EmitDropOneStack(Builder* b, LLVMValueRef slot, TypeDesc td);
void EmitDrops(Builder* b, size_t fromIndex);
void NoteFreshOwned(Builder* b, Value v);
const TempPart* FindTempPart(Builder* b, const Node* node);
void BorrowTempPart(Builder* b, Value base, const Node* baseNode, const Node* node, TypeDesc pieceTd,
                    LLVMValueRef heapAddr, LLVMTypeRef structTy, int fieldIdx);
void BorrowTemp(Builder* b, Value src, TypeDesc resultTd);
LLVMValueRef ClaimTemp(Builder* b, Value src);
LLVMValueRef ParkTemp(Builder* b, Value v);
void FlushTemps(Builder* b, size_t mark);
void ResetTemps(Builder* b);
Node* MovedSource(Node* n);
void NullMovedSource(Builder* b, Node* n);
LLVMValueRef EmitOwnedValue(Builder* b, Value evaluated, Node* init, const TypeName* innerType);
Value EmitCopyValue(Builder* b, Value src, TypeDesc td);
Value EmitCopyBuiltin(Builder* b, CallExpr* n);
Value EmitDropBuiltin(Builder* b, CallExpr* n);

/* Equality.c */
LLVMBasicBlockRef EmitBoxCellEq(Builder* b, LLVMValueRef fn, LLVMValueRef aCell, LLVMValueRef bCell,
                                const TypeDesc* innerTd, bool isOptional, LLVMBasicBlockRef failBB);
LLVMValueRef EnsureEqHelper(Builder* b, const TypeDesc* td, bool externBoxCtx);
LLVMValueRef SpillEqOperand(Builder* b, Value v);

/* Numeric.c */
LLVMValueRef BuildFloatToIntSaturating(Builder* b, LLVMValueRef value, LLVMTypeRef target, bool isUnsigned);
void EmitEnumCastCheck(Builder* b, CastExpr* cast, Value operand, TypeDesc target);
Value EmitBinary(Builder* b, BinaryExpr* n);

/* Vectors.c */
LValue MakeVectorLaneLValue(Builder* b, LValue none, const MemberExpr* m, LLVMValueRef vecAddr, TypeDesc vectorTd);
Value EmitVectorConstruct(Builder* b, CallExpr* n);
Value EmitVectorReduce(Builder* b, CallExpr* n);
Value EmitVectorDot(Builder* b, CallExpr* n);
Value EmitVectorCross(Builder* b, CallExpr* n);

/* Expr.c */
LLVMValueRef CurrentContext(Builder* b);
Value EmitImplProperty(Builder* b, MemberExpr* m, Node* valueNode, bool* found);
LLVMValueRef ToI1(Builder* b, Value v);
LValue EmitLValue(Builder* b, Node* n);
LLVMValueRef AsI64Index(Builder* b, Value v);
Value EmitCondOperand(Builder* b, Node* n);
LLVMValueRef SaveOldValue(Builder* b, LLVMValueRef slot, TypeDesc td);
void EmitDiscardExpr(Builder* b, Node* expr);
LLVMValueRef EmitConditionI1(Builder* b, Node* cond);

/* Call.c */
const FunctionDecl* FindModuleFunction(const Module* module, const char* name);
const char* StaticExprTypeName(Builder* b, Node* n);
LLVMValueRef ExternSlot(Builder* b, const char* name);
Value EmitCall(Builder* b, CallExpr* n);

/* Assign.c */
Value EmitAssign(Builder* b, AssignExpr* n);

/* Stmt.c */
void PushScope(Builder* b);
void PopScope(Builder* b);
BlockScope* TopScope(Builder* b);
void SymPush(Builder* b);
void SymPop(Builder* b);
void RunDefers(Builder* b, BlockScope* scope);
void EmitStmt(Builder* b, Node* n);

/* ConstInit.c */
ConstInitVal CastConstVal(Builder* b, ConstInitVal v, TypeDesc td);
bool FoldConstInit(Builder* b, TypeDesc td, Node* n, ConstInitVal* out);

/* Function.c */
bool IsAotEntryPoint(const Builder* b, const FunctionDecl* f);
void DeclareFunction(Builder* b, const FunctionDecl* f);
void DefineFunction(Builder* b, const FunctionDecl* f);
void EmitAotEntryPoint(Builder* b, const FunctionDecl* entry);

// Expr.c dispatch
Value EmitIntLiteral(Builder* b, IntLiteral* literal);
Value EmitFloatLiteral(Builder* b, FloatLiteral* literal);
Value EmitBoolLiteral(Builder* b, BoolLiteral* literal);
Value EmitIncDec(Builder* b, IncDecExpr* inc);
Value EmitCast(Builder* b, CastExpr* cast);
Value EmitStrLiteral(Builder* b, StrLiteral* literal);
Value EmitNullTest(Builder* b, NullTestExpr* nt);

// Stmt.c dispatch
void EmitFixedArrayVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc);
void EmitArrayVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc);
void EmitBoxVarDecl(Builder* b, VarDeclStmt* varDecl, TypeDesc typeDesc);
void SymDeclare(Builder* b, const char* name, Value* sym);

// Arrays.c dispatch (EmitArrayBuiltin cases)
Value EmitArrayPop(Builder* b, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd, LLVMTypeRef elemTy);
Value EmitArrayReserve(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, LLVMValueRef capPtr,
                       LLVMTypeRef elemTy);
Value EmitArrayRemove(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                      LLVMTypeRef elemTy);
Value EmitArrayReplace(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                       LLVMTypeRef elemTy);
Value EmitArrayTake(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, TypeDesc elemTd,
                    LLVMTypeRef elemTy);
Value EmitArrayPush(Builder* b, CallExpr* n, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr, LLVMValueRef capPtr,
                    TypeDesc elemTd, LLVMTypeRef elemTy);
Value EmitArrayResize(Builder* b, CallExpr* n, const TypeName* arrayInner, LLVMValueRef dataPtrPtr, LLVMValueRef lenPtr,
                      LLVMValueRef capPtr, TypeDesc elemTd, LLVMTypeRef elemTy);

// Arrays.c dispatch (EmitArrayFromNodes)
LLVMValueRef EmitArrayLiteralBuffer(Builder* b, LLVMTypeRef elemTy, LLVMTypeRef slotTy, size_t count, bool stackBuffer);
void EmitAliasedArrayElement(Builder* b, Node* eNode, Value v, LLVMValueRef elemAddr, TypeDesc elemTd,
                             LLVMTypeRef elemTy);
void EmitArrayElementStore(Builder* b, Node* eNode, Value v, LLVMValueRef elemAddr, TypeDesc elemTd, bool borrow);

// Expr.c dispatch (EmitLValue cases)
LValue EmitIdentLValue(Builder* b, IdentExpr* id);
LValue EmitMemberLValue(Builder* b, MemberExpr* m);
LValue EmitIndexLValue(Builder* b, IndexExpr* ix);
