#pragma once

#include "AST/AST.h"

#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"
#include "Core/Util.h"

#include "TypeRegistry.h"

#include "strata/strata.h"

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    Vec externSymbols;
} BuiltModule;

typedef struct
{
    LLVMTypeRef type;
    bool isFloat;
    bool isUnsigned;
    bool isVoid;
    const char* structTypeName;
    bool isBox;
    bool isOptional; /* T?: same repr as ^T, but may be empty (sema-gated) */
    const TypeName* boxInner; /* inner T of ^T / T? (NULL for string) */
    bool isArray;
    bool isSimdVector;
    const TypeName* arrayInner; /* element type of T[] / T[N] */
    bool aliasedArray;          /* ref T... rest: slots hold pointers to sources */
    bool isFixedArray;          /* T[N]: inline [N x T] (C ABI), struct fields only */
    long fixedLength;           /* N when isFixedArray */
} TypeDesc;

typedef struct
{
    LLVMValueRef value;
    TypeDesc typeDesc;
} Value;

typedef struct Builder
{
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
    StrMap m_dropFns; /* structName -> LLVMValueRef, per-type struct-field drop helper */
    Vec m_externNames;
    TypeDesc m_curRet;
    LLVMTypeRef m_curRetAbi; /* widened signature return type (i32 for sub-word ints) */
    bool m_terminated;
    bool m_jitMode;
    bool m_boundsCheck;     /* emit array bounds checks (StrataProfile) */
    bool m_nullExternCheck; /* panic on calling a null extern slot (StrataProfile) */
    bool m_nullStoreLValue; /* re-resolving an lvalue only to NULL it: skip OOB dummy re-init */
    LLVMValueRef m_curFn;
    LLVMBasicBlockRef m_entryBlock;
    LLVMValueRef m_entryAllocaPt;
    Vec m_loops;
    Vec m_owningLocals;
    LLVMTypeRef m_arrayType; /* cached {ptr, i64} fat struct for T[] */
    LLVMValueRef m_allocFn;
    LLVMTypeRef m_allocFnType;
    LLVMValueRef m_freeFn;
    LLVMTypeRef m_freeFnType;
    LLVMValueRef m_panicFn;
    LLVMTypeRef m_panicFnType;
    LLVMValueRef m_oobFn;      /* strata_oob: report OOB, continue (JIT) */
    LLVMTypeRef m_oobFnType;
    LLVMValueRef m_strdupFn;
    LLVMTypeRef m_strdupFnType;
    Arena* m_arena;
    int m_strLitCount;
} Builder;

void BuiltModuleInit(BuiltModule* bm);
void BuiltModuleDispose(BuiltModule* bm);

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                            const StrataProfile* profile);

Value EmitExpr(Builder* b, Node* n);
