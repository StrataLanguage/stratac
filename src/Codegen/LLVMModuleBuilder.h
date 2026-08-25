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
    const char* boxInner;
    bool isArray;
    bool isSimdVector;
    const char* arrayInner; /* element type name (arena-owned) */
    bool aliasedArray;      /* ref T... rest: slots hold pointers to sources */
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
    bool m_terminated;
    bool m_jitMode;
    bool m_boundsCheck;     /* emit array bounds checks (StrataProfile) */
    bool m_nullExternCheck; /* panic on calling a null extern slot (StrataProfile) */
    bool m_panicUnwind;     /* JIT-only: panics unwind the stack, dropping
                                owning locals frame by frame (StrataProfile) */
    bool m_nativeEntryWrappers; /* Windows x64 without TinyCC: the JIT'd-module
                                   wrappers are skipped; a separate native
                                   wrapper image (see BuildLlvmWrapperModule)
                                   provides registered-unwind boundaries */
    LLVMValueRef m_curFn;
    LLVMBasicBlockRef m_entryBlock;
    LLVMValueRef m_entryAllocaPt;
    LLVMValueRef m_curPadFrame; /* unwind frame of the function being defined;
                                   returns pop it so the TLS chain never holds
                                   stale frames (JIT unwind mode) */
    Vec m_loops;
    Vec m_owningLocals;
    size_t m_owningLocalsMax; /* high-water mark of m_owningLocals.count: the
                                 landing pad drops every slot ever registered
                                 (count itself is scope-truncated on block
                                 exit, so the pad can't rely on it) */
    LLVMTypeRef m_arrayType; /* cached {ptr, i64} fat struct for T[] */
    LLVMValueRef m_allocFn;
    LLVMTypeRef m_allocFnType;
    LLVMValueRef m_freeFn;
    LLVMTypeRef m_freeFnType;
    LLVMValueRef m_panicFn;
    LLVMTypeRef m_panicFnType;
    LLVMValueRef m_raiseFn;      /* __strata_raise(const char* msg) noreturn */
    LLVMTypeRef m_raiseFnType;
    LLVMValueRef m_pushFn;       /* __strata_unwind_push(void* frame) */
    LLVMTypeRef m_pushFnType;
    LLVMValueRef m_popFn;        /* __strata_unwind_pop_to(void* frame) */
    LLVMTypeRef m_popFnType;
    LLVMValueRef m_rethrowFn;    /* __strata_rethrow(void) noreturn */
    LLVMTypeRef m_rethrowFnType;
    LLVMValueRef m_panicMsgFn;   /* __strata_panic_message(void) -> char* */
    LLVMTypeRef m_panicMsgFnType;
    LLVMValueRef m_setjmpFn;     /* __strata_setjmp(void*) -> i32, returns_twice */
    LLVMTypeRef m_setjmpFnType;
    LLVMTypeRef m_unwindFrameTy; /* cached {[62 x i64], ptr, i64} frame struct */
    LLVMValueRef m_strdupFn;
    LLVMTypeRef m_strdupFnType;
    Arena* m_arena;
    int m_strLitCount;
} Builder;

void BuiltModuleInit(BuiltModule* bm);
void BuiltModuleDispose(BuiltModule* bm);

BuiltModule BuildLlvmModule(const Module* ast, DiagnosticEngine* diag, Arena* arena, bool jitMode,
                            const StrataProfile* profile);

/* Builds a module containing only the __strata_entry_<mangled> host-boundary
   wrappers: each real function is an external declaration the loader resolves
   against the LLVM JIT's addresses. Emitted to a native object and loaded by
   CoffImage (Windows x64 without TinyCC), so the wrapper code carries OS-
   registered unwind info (.pdata) and host longjmp/exception handlers can
   unwind through it. Returns an empty module when native wrappers are not
   the active configuration. */
BuiltModule BuildLlvmWrapperModule(const Module* ast, DiagnosticEngine* diag, Arena* arena);

Value EmitExpr(Builder* b, Node* n);
