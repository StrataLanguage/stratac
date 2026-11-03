#pragma once
// Resolver state and helpers shared by the Resolve/ source files.

#include "Codegen/TypeRegistry.h"
#include "Codegen/TypeUtil.h"
#include "Lex/Lexer.h"
#include "Sema/Builtins.h"
#include "Sema/ConstFold.h"
#include "Sema/MoveCheck.h"
#include "Sema/NodeTypes.h"
#include "Sema/ResolveOverloads.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    Module* mod;
    DiagnosticEngine* diag;
    TypeRegistry registry;
    NodeTypes nodeTypes; // the type InferType last gave each expression node
    Arena* arena;
    StrMap constVars;
    StrMap boxGlobals;
    StrMap refBoxParams;
    StrMap refLocals;      // `ref` locals: name -> the (canonical) path they alias
    StrMap refArrayParams; // `ref T[]` param names: borrowed binding — no rebind, no push/resize
    StrMap typeCache;      // canonical spelling -> interned TypeName tree
    StrMap constGlobals;   // const scalar global name -> ConstGlobalVal* (manifest constants)
    const TypeName* currentReturnType;
    const FunctionDecl* currentFunction;
    StrMap* curScope; // the function's scope, for typing move paths (see KeyPathType)

    // Vec of Vec*'s, a list of scopes and their respective symbols.
    Vec scopeDecls;

    // Vec of Vec*'s parallel to scopeDecls: each scope's DeferEntry*'s.
    Vec deferFrames;

    // Vec of LoopFlow*'s: break/continue targets of the enclosing loops.
    Vec loopFlows;

    // Nesting depth of deferred statements being walked.
    StackMark deferWalkDepth;

    // The current path is dead (after return/break/continue).
    bool unreachable;
} Resolver;

// A folded manifest constant: a `const` scalar global whose initializer is a
// compile-time constant. Emits no storage — the value is inlined at every
// use (the #define/enum alternative for C++ consumers).
typedef struct
{
    ConstInt i;
    double f;
    bool isFloat;
    bool isInt;       // integer-kind type: usable as a fixed-array dimension
    ConstIntType ity; // the runtime integer type `i` is normalized to (see ConstFold.h)
} ConstGlobalVal;

// What loops, `defer` and branches carry between paths: whether the path can be reached at all (no state
// yet, or every contributor was dead, makes it dead).
typedef struct
{
    bool live;
} FlowState;

typedef struct AllowList
{
    const char** allowed;
    int size;
} AllowList;

// Result of folding an enum member value expression.
typedef enum
{
    EnumEvalOk = 0,
    EnumEvalNotConst,       // not a constant integer expression (incl. div-by-zero, bad shift)
    EnumEvalNegForUnsigned, // a unary minus in an expression for an unsigned underlying
    EnumEvalForwardRef,     // references a member that isn't resolved yet (self/forward)
} EnumEvalStatus;

typedef struct IntrinsicTypeDefinition
{
    const char* name;
    const TypeName* (*typeFunc)(Resolver*, CallExpr*, StrMap*);
    bool (*resolveFunc)(Resolver*, CallExpr*, StrMap*);
} IntrinsicTypeDefinition;

// A `defer`red statement queued on its scope. It is checked against the flow
// state at every exit it runs at (block end, return, break, continue).
typedef struct
{
    Node* stmt;
    size_t frame;           // index of the owning scope in scopeDecls
    StackMark declMark;     // names the scope had declared at the `defer`
    DiagnosticEngine early; // diagnostics from the walk at the `defer` itself
    bool ranAtExit;
} DeferEntry;

// The break/continue targets of one loop iteration being walked.
typedef struct
{
    FlowState breaks;         // join of every `break`: part of the loop exit
    FlowState continues;      // join of every `continue`: a back edge
    StackMark deferDepth;     // deferFrames.count at body entry
    StackMark deferWalkDepth; // deferWalkDepth when the loop began
} LoopFlow;

// Upper bound on muted fixed-point passes per loop. The join is monotone
// over a finite key set so it converges long before this in practice.
#define LOOP_MAX_PASSES 32

// Functions defined in one Resolve source file and called from another.
ConstIntType SemaConstIntType(PrimitiveType p);
bool IsEnumUnderlyingType(const char* t);
void EnumSignedRange(const char* underlying, bool* isUnsigned, ConstInt* minVal, ConstUInt* maxVal);
bool EnumSignedValue(ConstUInt mag, bool isNegative, ConstInt* out);
bool ResolvedConstIntValue(Node* n, ConstUInt* mag, bool* isNegative);
bool EnumConstFits(const char* underlying, ConstUInt mag, bool isNegative);
void CheckLiteralFits(DiagnosticEngine* diag, const Node* value, const TypeName* target);
void CheckConstArith(DiagnosticEngine* diag, const BinaryExpr* b, const TypeName* lt);
void CheckConstFloatToInt(DiagnosticEngine* diag, const CastExpr* cast);
void CheckLossyConversion(DiagnosticEngine* diag, const Node* value, const TypeName* srcType, const TypeName* target,
                          SourceRange range);
bool ArgConvertsWithoutLoss(const Node* arg, const TypeName* argType, const TypeName* paramType);
bool SemaFoldConstInit(Resolver* r, Node* n, ConstGlobalVal* out);
bool IsConstDimType(const TypeRegistry* reg, const char* name);
bool IsManifestConstType(const TypeRegistry* reg, const char* name);
bool IsBoolType(const TypeRegistry* reg, const TypeName* t);
bool IsBoolContextType(const TypeRegistry* reg, const TypeName* t);
bool IsImplicitScalarConversion(PrimitiveType target, PrimitiveType value);
bool SemaResolveConstDims(Resolver* r, TypeName* t);
const TypeName* InternTypeName(Resolver* r, const char* name);
char* Mangle(Arena* arena, const FunctionDecl* f);
bool IsDefinedStruct(const TypeRegistry* reg, const char* name);
bool IsIncompleteStruct(const TypeRegistry* reg, const char* name);
void ResetStrMap(StrMap* m);
size_t NamedParamCount(const FunctionDecl* f);
bool TypeTreeHasFixedArray(const TypeName* t);
const TypeName* FixedArrayLeaf(const TypeName* t, ArrayLength* totalCount);
const TypeName* UnwrapBoxPtr(const TypeName* t);
bool IsFatPseudoProperty(const Resolver* r, const TypeName* base, const char* member);
bool PlaceFixedArrayInit(Vec* flat, size_t base, const TypeName* t, ArrayInitExpr* ai);
bool CheckFixedArrayInitShape(Resolver* r, const TypeName* t, ArrayInitExpr* ai, const TypeName* leaf, const char* name,
                              const char* kind);
bool HasNestedArrayInit(const ArrayInitExpr* ai);
bool AliasIsOwning(const Resolver* r, const TypeName* t);
bool AliasIsOwningValue(const Resolver* r, const TypeName* t);
bool ResolvesToDynamicArray(const Resolver* r, const TypeName* t);
bool SameResolvedType(const Resolver* r, const char* a, const char* b);
bool IsBoxGlobalName(const Resolver* r, const char* name);
const char* CanonicalRefKey(Resolver* r, const char* key);
void MarkMoved(Resolver* r, const char* name, SourceRange range);
void MoveOptionalSource(Resolver* r, const char* name, SourceRange range);
void EmptyOptionalSource(Resolver* r, const char* name);
const char* MovableBoxSourceKey(Resolver* r, Node* n);
void FlowInit(FlowState* s);
void FlowFree(FlowState* s);
void FlowCapture(const Resolver* r, FlowState* s);
void FlowApply(Resolver* r, const FlowState* s);
void FlowJoin(FlowState* dst, const FlowState* src);
void FlowJoinCurrent(const Resolver* r, FlowState* dst);
bool FlowSame(const FlowState* a, const FlowState* b);
int CountByName(const Module* mod, const char* name);
bool IsLengthPseudoMemberBase(const TypeRegistry* reg, const TypeName* t);
const char* ConstRootName(const Resolver* r, Node* target, StrMap* scope);
bool CheckAllowListSingle(Resolver* r, CallExpr* c, StrMap* scope, const char** allowed, const int allowedSize);
bool CheckArgTypesUnordered(Resolver* r, CallExpr* c, StrMap* scope, const AllowList* allowed, const int allowedSize);
bool ResolveVectorConstruct(Resolver* r, CallExpr* c, StrMap* scope);
bool FunctionVisibleTo(const FunctionDecl* fd, uint16_t callerFile);
bool ModuleHasFunctionNamed(Resolver* r, const char* name, uint16_t callerFile);
bool ResolveVector2Arg(Resolver* r, CallExpr* c, StrMap* scope);
bool ResolveVector1Arg(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* CopyBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
bool ResolveCopyBuiltin(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* SubstringBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
bool ResolveSubstringBuiltin(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* DropBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
bool ResolveDropBuiltin(Resolver* r, CallExpr* c, StrMap* scope);
bool IsTypedMemoryIntrinsic(Resolver* r, const CallExpr* c);
bool IsStringConcat(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt);
const char* ExprEnumName(Resolver* r, const Node* n, const TypeName* t);
bool IsEnumFlagsOp(Resolver* r, BinaryOp op, const TypeName* lt, const TypeName* rt);
bool IsPlainEnum(const Resolver* r, const char* name);
void InferBorrowedParams(Resolver* r);
void MarkGrowableArrayParams(Module* mod);
bool IsGrowableArrayParam(const Resolver* r, const char* name);
void CheckGrowableArrayArgs(Resolver* r, const FunctionDecl* best, CallExpr* c, StrMap* scope);
const TypeName* ConditionalType(Resolver* r, ConditionalExpr* ce, StrMap* scope, bool diagnose);
void CheckDelegateDecl(Resolver* r, const FunctionDecl* f);
bool IsCAbiScalar(Resolver* r, const TypeName* t);
void CheckExportDecl(Resolver* r, const FunctionDecl* f);
FunctionDecl* FindSingleFunction(Resolver* r, const char* name, uint16_t fileId, bool* ambiguous);
bool IsAddressOfFunctionArg(Resolver* r, const CallExpr* c, StrMap* scope);
const TypeName* MemoryBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
bool ResolveMemoryBuiltin(Resolver* r, CallExpr* c, StrMap* scope);
void DecayFixedArgsToPtr(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* ArrayBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
bool IsEmptyBracedForOptional(const Node* n, const TypeName* target);
bool IsArrayElementBorrow(Node* n);
bool IsStrLiteralForCString(const Resolver* r, const TypeName* targetType, const Node* valueNode);
bool ResolveArrayBuiltin(Resolver* r, CallExpr* c, StrMap* scope);
bool IsCVarargCompatible(Resolver* r, const TypeName* type);
void TrackCallArgMoves(Resolver* r, const FunctionDecl* best, CallExpr* c);
void CheckCallArgOptionalDerefs(Resolver* r, CallExpr* c, StrMap* scope);
Node* ApplyBracedStructTarget(Resolver* r, Node* node, const TypeName* target);
void CheckStructInitFields(Resolver* r, Node* n);
FunctionDecl* FindImplMethod(Resolver* r, const char* handleName, const char* methodName);
PropertyDecl* PropertyOnHandle(Resolver* r, const TypeName* baseType, const char* member);
bool TryResolveEnumMember(Resolver* r, MemberExpr* m, StrMap* scope);
bool TryResolveScalarPseudoConst(Resolver* r, MemberExpr* m);
bool ResolveMemberCall(Resolver* r, CallExpr* c, StrMap* scope);
BitWidth ConstBitWidth(const Node* n);
void ResolveEnums(Module* mod, DiagnosticEngine* diag, const TypeRegistry* registry);
void ResolveImpls(Module* mod, DiagnosticEngine* diag, Arena* arena, const TypeRegistry* registry);
bool OverloadTakesFixedView(Resolver* r, const char* name, const TypeName* fixedType, int fileId);
void StampArrayInitElemTypes(ArrayInitExpr* ai, const TypeName* elem);
void CheckGenericExternDecl(Resolver* r, const FunctionDecl* f);
void ResolveCall(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* VectorConstructBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* VectorDotBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* VectorCrossBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* VectorReduceBuiltinType(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* InferIntrinsicType(Resolver* r, CallExpr* c, StrMap* scope);
const TypeName* InferType(Resolver* r, Node* n, StrMap* scope);
bool IsVectorAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType,
                            const char* resolvedTarget, const char* resolvedValue);
bool IsAssignableType(const Resolver* r, const TypeName* targetType, const TypeName* valueType);
bool ZeroIsValid(Resolver* r, const TypeName* t, int depth);
void ResolveArrayRepeat(Resolver* r, ArrayInitExpr* ai, StrMap* scope);
void ResolveRefLocal(Resolver* r, VarDeclStmt* vd, StrMap* scope);
void ResolveExpr(Resolver* r, Node* n, StrMap* scope);
void CheckBoolCondition(Resolver* r, Node* cond, StrMap* scope);
void ScopeDeclare(Resolver* r, StrMap* scope, const char* name, void* typePtr, SourceRange range);
void WalkBlock(Resolver* r, Block* b, StrMap* scope);
bool StmtFallsThrough(const Node* n, int depth);

// Expr.c dispatch: ResolveExprImpl hands these node kinds to their feature files.
void ResolveExprImpl(Resolver* r, Node* n, StrMap* scope, bool asMemberBase);
void ResolveMemberExpr(Resolver* r, Node* n, StrMap* scope);
void ResolveStructInitExpr(Resolver* r, Node* n, StrMap* scope);
void ResolveIndexExpr(Resolver* r, Node* n, StrMap* scope);
void ResolveArrayInitExpr(Resolver* r, Node* n, StrMap* scope);
void ResolveCallExpr(Resolver* r, Node* n, StrMap* scope);
