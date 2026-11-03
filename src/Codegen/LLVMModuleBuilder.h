#pragma once

#include "AST/AST.h"

#include "Codegen/LLVMCApi.h"
#include "Core/Diagnostics.h"
#include "Core/Util.h"

#include "TypeRegistry.h"

#include "strata/strata.h"

typedef struct
{
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    Vec externSymbols;
    bool hasInstancedGlobals; /* module has >=1 storage-backed global, so __strata_context_create
                                  returns a real context instead of NULL */
    uint8_t* typeMetadata;    /* strata_types.h blob (malloc'd), NULL when the module has no components */
    size_t typeMetadataSize;
} BuiltModule;

/* The shape bits of a TypeDesc. Read and written through the TypeDescIs / TypeDescSet accessors in
   Codegen/TypeDescFlags.h. */
typedef enum TypeDescFlags
{
    TDF_NONE = (0),
    TDF_FLOAT = (1 << 0),
    TDF_UNSIGNED = (1 << 1),
    TDF_VOID = (1 << 2),
    TDF_BOX = (1 << 3),
    TDF_OPTIONAL = (1 << 4), /* T?: same repr as ^T, but may be empty (sema-gated) */
    TDF_ARRAY = (1 << 5),
    TDF_STRING = (1 << 6),  /* string / alias-of-string: fat {ptr, len, cap} with a NUL at [len] */
    TDF_CSTRING = (1 << 7), /* cstring / alias-of-cstring: single `const char*` to `.rodata` (borrowed) */
    TDF_SIMD_VECTOR = (1 << 8),
    TDF_ALIASED_ARRAY = (1 << 9), /* ref T... rest: slots hold pointers to sources */
    TDF_FIXED_ARRAY = (1 << 10),  /* T[N]: inline [N x T] (C ABI) — struct fields and stack-allocated locals */
} TypeDescFlags;

typedef struct
{
    LLVMTypeRef type;
    TypeDescFlags flags; /* shape bits; read and written through the TypeDescIs / TypeDescSet accessors */
    const char* structTypeName;
    const TypeName* boxInner;   /* inner T of ^T / T? (NULL for string) */
    LaneCount simdLanes;        /* logical lanes of a SIMD vector (2/3/4), 0 if unknown: float3 shares
                                   float4's 4-lane LLVM vector, so horizontal ops must not read lane 3 */
    const TypeName* arrayInner; /* element type of T[] / T[N] */
    ArrayLength fixedLength;    /* N when TDF_FIXED_ARRAY */
} TypeDesc;

typedef struct
{
    LLVMValueRef value;
    TypeDesc typeDesc;
} Value;

typedef struct Builder
{
    DiagnosticEngine* diag;
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMTypeRef ptrTy;
    TypeRegistry registry;
    StrMap structTypes;
    StrMap funcs;
    StrMap symbols;
    StrMap globals;
    StrMap constValues;           /* manifest-const global name -> ConstValueSlot* (no storage emitted) */
    StrMap globalFieldIndex;      /* storage-backed global name -> boxed (field index + 1) in globalsStructTy */
    bool hasInstancedGlobals;     /* module has >=1 storage-backed global (see BuiltModule.hasInstancedGlobals) */
    LLVMTypeRef globalsStructTy;  /* per-instance "context" struct holding every instanced global */
    LLVMValueRef exportCtxGlobal; /* module-private slot: the context `@export` bodies use (the newest
                                       created one); NULL without instanced globals */
    LLVMValueRef curGlobalsPtr;   /* current function's own hidden context pointer (NULL if none) */
    StrMap externSlots;
    StrMap implProps; /* "Handle.Prop" -> ImplPropEntry (getter/setter lowering) */
    StrMap dropFns;   /* structName -> LLVMValueRef, per-type struct-field drop helper */
    StrMap copyFns;   /* structName -> LLVMValueRef, per-type struct deep-copy helper */
    Vec externNames;
    TypeDesc curRet;
    LLVMTypeRef curRetAbi; /* widened signature return type (i32 for sub-word ints) */
    bool terminated;
    bool jitMode;
    bool boundsCheck;       /* emit array bounds checks (StrataProfile) */
    bool nullExternCheck;   /* panic on calling a null extern slot (StrataProfile) */
    bool nullStoreLValue;   /* re-resolving an lvalue only to NULL it: skip OOB dummy re-init */
    bool discardCallResult; /* the expression statement's direct call result is unused */
    LLVMValueRef curFn;
    LLVMBasicBlockRef entryBlock;
    LLVMValueRef entryAllocaPt;
    Vec loops;
    Vec owningLocals;
    Vec temps;             /* OwnLocal*: borrowed owning temporaries, dropped at the end of the full statement */
    Vec freshOwned;        /* LLVMValueRef: owning call results of the current statement not yet bound/borrowed */
    Vec tempParts;         /* TempPart*: owning pieces read out of a registered temporary, nulled if moved out */
    Vec scopes;            /* active block scopes for `defer` (BlockScope*) */
    Vec symDecls;          /* stack of Vec*: locals declared in each active lexical block */
    LLVMTypeRef arrayType; /* cached {ptr, u32 len, u32 cap} fat struct for T[] */
    LLVMValueRef emptyNul; /* cached static "" (single NUL byte) for extern string puns */
    LLVMValueRef allocFn;
    LLVMTypeRef allocFnType;
    LLVMValueRef freeFn;
    LLVMTypeRef freeFnType;
    LLVMValueRef panicFn;
    LLVMTypeRef panicFnType;
    LLVMValueRef oobFn; /* strata_oob: report OOB, continue (JIT) */
    LLVMTypeRef oobFnType;
    LLVMValueRef strdupFn;
    LLVMTypeRef strdupFnType;
    LLVMValueRef strEqFn; /* strata_str_eq: content equality for string ==/!= */
    LLVMTypeRef strEqFnType;
    LLVMValueRef csLenFn; /* strata_cstrlen: NUL-terminated length for cstring ==/!= */
    LLVMTypeRef csLenFnType;
    StrMap eqHelpers;
    StrMap structDefaults;    /* structName -> LLVMValueRef constant with field defaults applied; absent = all zero */
    StrMap typeDescs;         /* structName -> LLVMValueRef StrataTypeDesc global passed to `extern<T>` calls */
    const char* symbolPrefix; /* prepended to every exported (non-extern) symbol; NULL = none */
    const Module* module;     /* the module being built */
    Arena* arena;
    int strLitCount;
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
