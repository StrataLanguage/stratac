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
    bool hasInstancedGlobals; /* module has >=1 storage-backed global, so __strata_context_create
                                  returns a real context instead of NULL */
    uint8_t* typeMetadata;    /* strata_types.h blob (malloc'd), NULL when the module has no components */
    size_t typeMetadataSize;
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
    bool isString;              /* string / alias-of-string: fat {ptr, len, cap} with a NUL at [len] */
    bool isCString;             /* cstring / alias-of-cstring: single `const char*` to `.rodata` (borrowed) */
    bool isSimdVector;
    int simdLanes;              /* logical lanes of a SIMD vector (2/3/4), 0 if unknown: float3 shares
                                   float4's 4-lane LLVM vector, so horizontal ops must not read lane 3 */
    const TypeName* arrayInner; /* element type of T[] / T[N] */
    bool aliasedArray;          /* ref T... rest: slots hold pointers to sources */
    bool isFixedArray;          /* T[N]: inline [N x T] (C ABI) — struct fields and stack-allocated locals */
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
    StrMap m_constValues; /* manifest-const global name -> ConstValueSlot* (no storage emitted) */
    StrMap m_globalFieldIndex; /* storage-backed global name -> boxed (field index + 1) in m_globalsStructTy */
    bool m_hasInstancedGlobals; /* module has >=1 storage-backed global (see BuiltModule.hasInstancedGlobals) */
    LLVMTypeRef m_globalsStructTy; /* per-instance "context" struct holding every instanced global */
    LLVMValueRef m_curGlobalsPtr; /* current function's own hidden context pointer (NULL if none) */
    StrMap m_externSlots;
    StrMap m_implProps; /* "Handle.Prop" -> ImplPropEntry (getter/setter lowering) */
    StrMap m_dropFns; /* structName -> LLVMValueRef, per-type struct-field drop helper */
    StrMap m_copyFns; /* structName -> LLVMValueRef, per-type struct deep-copy helper */
    Vec m_externNames;
    TypeDesc m_curRet;
    LLVMTypeRef m_curRetAbi; /* widened signature return type (i32 for sub-word ints) */
    bool m_terminated;
    bool m_jitMode;
    bool m_boundsCheck;     /* emit array bounds checks (StrataProfile) */
    bool m_nullExternCheck; /* panic on calling a null extern slot (StrataProfile) */
    bool m_nullStoreLValue; /* re-resolving an lvalue only to NULL it: skip OOB dummy re-init */
    bool m_discardCallResult; /* the expression statement's direct call result is unused */
    LLVMValueRef m_curFn;
    LLVMBasicBlockRef m_entryBlock;
    LLVMValueRef m_entryAllocaPt;
    Vec m_loops;
    Vec m_owningLocals;
    Vec m_temps;         /* OwnLocal*: borrowed owning temporaries, dropped at the end of the full statement */
    Vec m_freshOwned;    /* LLVMValueRef: owning call results of the current statement not yet bound/borrowed */
    Vec m_tempParts;     /* TempPart*: owning pieces read out of a registered temporary, nulled if moved out */
    Vec m_scopes;        /* active block scopes for `defer` (BlockScope*) */
    Vec m_symDecls;      /* stack of Vec*: locals declared in each active lexical block */
    LLVMTypeRef m_arrayType; /* cached {ptr, u32 len, u32 cap} fat struct for T[] */
    LLVMValueRef m_emptyNul; /* cached static "" (single NUL byte) for extern string puns */
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
    LLVMValueRef m_strEqFn; /* strata_str_eq: content equality for string ==/!= */
    LLVMTypeRef m_strEqFnType;
    LLVMValueRef m_csLenFn; /* strata_cstrlen: NUL-terminated length for cstring ==/!= */
    LLVMTypeRef m_csLenFnType;
    StrMap m_eqHelpers;
    StrMap m_structDefaults; /* structName -> LLVMValueRef constant with field defaults applied; absent = all zero */
    StrMap m_typeDescs;      /* structName -> LLVMValueRef StrataTypeDesc global passed to `extern<T>` calls */
    const char* m_symbolPrefix; /* prepended to every exported (non-extern) symbol; NULL = none */
    const Module* m_module;     /* the module being built */
    Arena* m_arena;
    int m_strLitCount;
} Builder;

void BuiltModuleInit(BuiltModule* bm);
void BuiltModuleDispose(BuiltModule* bm);

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                            const StrataProfile* profile);

/* As BuildLlvmModule; `symbolPrefix` (NULL or "" = none) goes in front of every exported symbol,
   so several AOT objects can link into one binary (see strataSetSymbolPrefix). */
BuiltModule BuildLlvmModuleEx(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                              const StrataProfile* profile, const char* symbolPrefix);

Value EmitExpr(Builder* b, Node* n);
