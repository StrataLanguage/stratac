// Strata compiler: curated forward declarations for the LLVM C API.
//
// The official Windows LLVM distribution does not ship the full llvm-c headers,
// and the rest of LLVM's header graph (llvm/Config, llvm/Support) is heavy to
// vendor. Strata consumes only a small slice of the C API, so we declare exactly
// that slice here, matching the signatures in llvm/include/llvm-c/Core.h for the
// LLVM build we link against (see LLVM_C_DIR / LLVMGetVersion at runtime).
//
// All opaque types are declared as incomplete structs; the link target is the
// LLVM-C shared library (LLVM-C.dll on Windows), reached through its import
// library (LLVM-C.lib). The compiler's own C++ code is MinGW-built; only the C
// ABI crosses that boundary, which is stable across toolchains.
//
// IMPORTANT: keep these signatures byte-for-byte consistent with the upstream
// C headers. When adopting more of the API, copy the declaration verbatim and
// add it here.
#pragma once

#include <cstdint>

// Opaque handle typedefs (subset defined as needed).
extern "C"
{
    struct LLVMOpaqueContext;
    struct LLVMOpaqueModule;
    struct LLVMOpaqueType;
    struct LLVMOpaqueValue;
    struct LLVMOpaqueBasicBlock;
    struct LLVMOpaqueBuilder;

    using LLVMContextRef = struct LLVMOpaqueContext*;
    using LLVMModuleRef = struct LLVMOpaqueModule*;
    using LLVMTypeRef = struct LLVMOpaqueType*;
    using LLVMValueRef = struct LLVMOpaqueValue*;
    using LLVMBasicBlockRef = struct LLVMOpaqueBasicBlock*;
    using LLVMBuilderRef = struct LLVMOpaqueBuilder*;

    // llvm-c/Core.h
    using LLVMBool = int;

    // --- Version / context / module ---
    void LLVMGetVersion(unsigned* major, unsigned* minor, unsigned* patch);

    LLVMContextRef LLVMContextCreate(void);
    void LLVMContextDispose(LLVMContextRef c);

    LLVMModuleRef LLVMModuleCreateWithNameInContext(const char* moduleId, LLVMContextRef c);
    void LLVMDisposeModule(LLVMModuleRef m);
    void LLVMDumpModule(LLVMModuleRef m);
    char* LLVMPrintModuleToString(LLVMModuleRef m);
    void LLVMDisposeMessage(char* message);

    // --- Types ---
    LLVMTypeRef LLVMInt1Type(void);
    LLVMTypeRef LLVMInt32Type(void);
    LLVMTypeRef LLVMInt64Type(void);
    LLVMTypeRef LLVMHalfType(void);
    LLVMTypeRef LLVMFloatType(void);
    LLVMTypeRef LLVMDoubleType(void);
    LLVMTypeRef LLVMVoidType(void);
    LLVMTypeRef LLVMVectorType(LLVMTypeRef elementType, unsigned elementCount);

    // Context-specific constructors (preferred; the context-less versions above
    // build in LLVM's global context, which must never be mixed with a module's
    // own context).
    LLVMTypeRef LLVMInt1TypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMInt32TypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMInt64TypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMHalfTypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMFloatTypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMDoubleTypeInContext(LLVMContextRef c);
    LLVMTypeRef LLVMVoidTypeInContext(LLVMContextRef c);

    // Struct types (user-defined types).
    LLVMTypeRef LLVMStructCreateNamed(LLVMContextRef c, const char* name);
    LLVMBool LLVMStructSetBody(LLVMTypeRef structTy, LLVMTypeRef* elementTypes, unsigned elementCount, LLVMBool packed);

    LLVMTypeRef LLVMFunctionType(LLVMTypeRef returnType, LLVMTypeRef* paramTypes, unsigned paramCount,
                                 LLVMBool isVarArg);

    // --- Constants & params ---
    LLVMValueRef LLVMConstInt(LLVMTypeRef intTy, unsigned long long n, LLVMBool signExtend);
    LLVMValueRef LLVMConstReal(LLVMTypeRef ty, double v);
    LLVMValueRef LLVMGetParam(LLVMValueRef func, unsigned index);

    // Globals (used for the JIT's extern-call slots).
    LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef c, unsigned addressSpace);
    LLVMValueRef LLVMConstNull(LLVMTypeRef ty);
    LLVMValueRef LLVMAddGlobal(LLVMModuleRef m, LLVMTypeRef ty, const char* name);
    void LLVMSetInitializer(LLVMValueRef globalVar, LLVMValueRef constantVal);

    // --- Functions & basic blocks ---
    LLVMValueRef LLVMAddFunction(LLVMModuleRef m, const char* name, LLVMTypeRef functionTy);
    LLVMBasicBlockRef LLVMAppendBasicBlock(LLVMValueRef func, const char* name);
    LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef c, LLVMValueRef func, const char* name);

    // --- Builder ---
    LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef c);
    void LLVMDisposeBuilder(LLVMBuilderRef b);
    void LLVMPositionBuilderAtEnd(LLVMBuilderRef b, LLVMBasicBlockRef block);

    LLVMValueRef LLVMBuildRet(LLVMBuilderRef b, LLVMValueRef v);
    LLVMValueRef LLVMBuildRetVoid(LLVMBuilderRef b);
    LLVMValueRef LLVMBuildBr(LLVMBuilderRef b, LLVMBasicBlockRef dest);
    LLVMValueRef LLVMBuildCondBr(LLVMBuilderRef b, LLVMValueRef If, LLVMBasicBlockRef then, LLVMBasicBlockRef Else);
    LLVMValueRef LLVMBuildAdd(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildSub(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildMul(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildFAdd(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildFSub(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildFMul(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildAlloca(LLVMBuilderRef b, LLVMTypeRef ty, const char* name);
    LLVMValueRef LLVMBuildStore(LLVMBuilderRef b, LLVMValueRef val, LLVMValueRef ptr);
    LLVMValueRef LLVMBuildLoad2(LLVMBuilderRef b, LLVMTypeRef ty, LLVMValueRef ptr, const char* name);
    LLVMValueRef LLVMBuildCall2(LLVMBuilderRef b, LLVMTypeRef fnTy, LLVMValueRef fn, LLVMValueRef* args,
                                unsigned numArgs, const char* name);

    // Additional arithmetic / logic / cast builders.
    using LLVMIntPredicate = enum
    {
        LLVMIntEQ = 32,
        LLVMIntNE,
        LLVMIntUGT,
        LLVMIntUGE,
        LLVMIntULT,
        LLVMIntULE,
        LLVMIntSGT,
        LLVMIntSGE,
        LLVMIntSLT,
        LLVMIntSLE
    };

    using LLVMRealPredicate = enum
    {
        LLVMRealPredicateFalse = 0,
        LLVMRealOEQ = 1,
        LLVMRealOGT = 2,
        LLVMRealOGE = 3,
        LLVMRealOLT = 4,
        LLVMRealOLE = 5,
        LLVMRealONE = 6,
        LLVMRealORD = 7,
        LLVMRealUNO = 8,
        LLVMRealUEQ = 9,
        LLVMRealUGT = 10,
        LLVMRealUGE = 11,
        LLVMRealULT = 12,
        LLVMRealULE = 13,
        LLVMRealUNE = 14,
        LLVMRealPredicateTrue = 15
    };

    LLVMValueRef LLVMBuildSDiv(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildUDiv(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildFDiv(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildSRem(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildURem(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildFRem(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildShl(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildLShr(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildAShr(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildAnd(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildOr(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildXor(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildNeg(LLVMBuilderRef b, LLVMValueRef v, const char* name);
    LLVMValueRef LLVMBuildNot(LLVMBuilderRef b, LLVMValueRef v, const char* name);
    LLVMValueRef LLVMBuildFNeg(LLVMBuilderRef b, LLVMValueRef v, const char* name);
    LLVMValueRef LLVMBuildICmp(LLVMBuilderRef b, LLVMIntPredicate pred, LLVMValueRef l, LLVMValueRef r,
                               const char* name);
    LLVMValueRef LLVMBuildFCmp(LLVMBuilderRef b, LLVMRealPredicate pred, LLVMValueRef l, LLVMValueRef r, const char* name);
    LLVMValueRef LLVMBuildSIToFP(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
    LLVMValueRef LLVMBuildUIToFP(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
    LLVMValueRef LLVMBuildFPToSI(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
    LLVMValueRef LLVMBuildFPToUI(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
    LLVMValueRef LLVMBuildIntCast2(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, LLVMBool isSigned,
                                   const char* name);

    // Aggregate (struct) construction and member access.
    LLVMValueRef LLVMGetUndef(LLVMTypeRef ty);
    LLVMValueRef LLVMBuildInsertValue(LLVMBuilderRef b, LLVMValueRef agg, LLVMValueRef val, unsigned index,
                                      const char* name);
    LLVMValueRef LLVMBuildExtractValue(LLVMBuilderRef b, LLVMValueRef agg, unsigned index, const char* name);
    LLVMValueRef LLVMBuildGEP2(LLVMBuilderRef b, LLVMTypeRef ty, LLVMValueRef pointer, LLVMValueRef* indices,
                               unsigned numIndices, const char* name);

    // --- Verification ---
    // LLVMVerifierFailureAction: LLVMAbortProcessAction = 0, LLVMReturnStatusAction = 1.
    LLVMBool LLVMVerifyModule(LLVMModuleRef m, int verifierAction, char** outMessage);

    // --- Target initialization ---
    // The LLVMInitializeAll* / LLVMInitializeNativeTarget convenience wrappers are
    // not exported by this LLVM-C.dll, but the per-target entry points are. Both
    // X86 (host) and AArch64 (cross-compile target) are initialized so stratac can
    // emit native code for either architecture.
    void LLVMInitializeX86TargetInfo(void);
    void LLVMInitializeX86Target(void);
    void LLVMInitializeX86TargetMC(void);
    void LLVMInitializeX86AsmPrinter(void);

    void LLVMInitializeAArch64TargetInfo(void);
    void LLVMInitializeAArch64Target(void);
    void LLVMInitializeAArch64TargetMC(void);
    void LLVMInitializeAArch64AsmPrinter(void);

    // --- Targets & target machines (llvm-c/Target.h, TargetMachine.h) ---
    using LLVMTargetRef = struct LLVMOpaqueTarget*;
    using LLVMTargetMachineRef = struct LLVMOpaqueTargetMachine*;
    using LLVMMemoryBufferRef = struct LLVMOpaqueMemoryBuffer*;

    using LLVMCodeGenOptLevel = enum
    {
        LLVMCodeGenLevelNone = 0,
        LLVMCodeGenLevelLess = 1,
        LLVMCodeGenLevelDefault = 2,
        LLVMCodeGenLevelAggressive = 3
    };

    using LLVMRelocMode = enum
    {
        LLVMRelocDefault = 0,
        LLVMRelocStatic = 1,
        LLVMRelocPIC = 2,
        LLVMRelocDynamicNoPic = 3
    };

    using LLVMCodeModel = enum
    {
        LLVMCodeModelDefault = 0,
        LLVMCodeModelJITDefault = 1,
        LLVMCodeModelTiny = 2,
        LLVMCodeModelSmall = 3,
        LLVMCodeModelKernel = 4,
        LLVMCodeModelMedium = 5,
        LLVMCodeModelLarge = 6
    };

    using LLVMCodeGenFileType = enum
    {
        LLVMAssemblyFile = 0,
        LLVMObjectFile = 1
    };

    LLVMBool LLVMGetTargetFromTriple(const char* triple, LLVMTargetRef* target, char** errorMessage);
    char* LLVMGetDefaultTargetTriple(void);
    char* LLVMGetHostCPUName(void);
    LLVMTargetMachineRef LLVMCreateTargetMachine(LLVMTargetRef t, const char* triple, const char* cpu,
                                                 const char* features, LLVMCodeGenOptLevel level, LLVMRelocMode reloc,
                                                 LLVMCodeModel codeModel);
    void LLVMDisposeTargetMachine(LLVMTargetMachineRef t);

    using LLVMTargetDataRef = struct LLVMOpaqueTargetData*;
    
    LLVMTargetDataRef LLVMCreateTargetDataLayout(LLVMTargetMachineRef t);

    char* LLVMCopyStringRepOfTargetData(LLVMTargetDataRef td);
    void LLVMDisposeTargetData(LLVMTargetDataRef td);
    void LLVMSetDataLayout(LLVMModuleRef m, const char* dataLayout);
    void LLVMSetTarget(LLVMModuleRef m, const char* triple);

    LLVMBool LLVMTargetMachineEmitToFile(LLVMTargetMachineRef t, LLVMModuleRef m, const char* filename,
                                         LLVMCodeGenFileType codegen, char** errorMessage);
    LLVMBool LLVMTargetMachineEmitToMemoryBuffer(LLVMTargetMachineRef t, LLVMModuleRef m, LLVMCodeGenFileType codegen,
                                                 char** errorMessage, LLVMMemoryBufferRef* outMemBuf);
    void LLVMDisposeMemoryBuffer(LLVMMemoryBufferRef memBuf);

    // --- Execution engine / MCJIT (llvm-c/ExecutionEngine.h) ---
    using LLVMExecutionEngineRef = struct LLVMOpaqueExecutionEngine*;
    LLVMBool LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef* outEe, LLVMModuleRef m, char** outError);
    void LLVMDisposeExecutionEngine(LLVMExecutionEngineRef ee);
    uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef ee, const char* name);
    uint64_t LLVMGetGlobalValueAddress(LLVMExecutionEngineRef ee, const char* name);

    // Symbol resolution for the JIT: binds a declared function/global in `M` to a
    // host address. Must be called on the engine before the referencing code is
    // compiled (i.e. before LLVMGetFunctionAddress triggers finalization).
    LLVMValueRef LLVMGetNamedFunction(LLVMModuleRef m, const char* name);
    void LLVMAddGlobalMapping(LLVMExecutionEngineRef ee, LLVMValueRef global, void* addr);
} // extern "C"

namespace strata::llvm_c
{
constexpr int kReturnStatusAction = 1;
}
