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
extern "C" {
struct LLVMOpaqueContext;
struct LLVMOpaqueModule;
struct LLVMOpaqueType;
struct LLVMOpaqueValue;
struct LLVMOpaqueBasicBlock;
struct LLVMOpaqueBuilder;

typedef struct LLVMOpaqueContext*    LLVMContextRef;
typedef struct LLVMOpaqueModule*     LLVMModuleRef;
typedef struct LLVMOpaqueType*       LLVMTypeRef;
typedef struct LLVMOpaqueValue*      LLVMValueRef;
typedef struct LLVMOpaqueBasicBlock* LLVMBasicBlockRef;
typedef struct LLVMOpaqueBuilder*    LLVMBuilderRef;

// llvm-c/Core.h
typedef int LLVMBool;

// --- Version / context / module ---
void LLVMGetVersion(unsigned* Major, unsigned* Minor, unsigned* Patch);

LLVMContextRef LLVMContextCreate(void);
void LLVMContextDispose(LLVMContextRef C);

LLVMModuleRef LLVMModuleCreateWithNameInContext(const char* ModuleID, LLVMContextRef C);
void LLVMDisposeModule(LLVMModuleRef M);
void LLVMDumpModule(LLVMModuleRef M);
char* LLVMPrintModuleToString(LLVMModuleRef M);
void LLVMDisposeMessage(char* Message);

// --- Types ---
LLVMTypeRef LLVMInt1Type(void);
LLVMTypeRef LLVMInt32Type(void);
LLVMTypeRef LLVMInt64Type(void);
LLVMTypeRef LLVMHalfType(void);
LLVMTypeRef LLVMFloatType(void);
LLVMTypeRef LLVMDoubleType(void);
LLVMTypeRef LLVMVoidType(void);
LLVMTypeRef LLVMVectorType(LLVMTypeRef ElementType, unsigned ElementCount);

// Context-specific constructors (preferred; the context-less versions above
// build in LLVM's global context, which must never be mixed with a module's
// own context).
LLVMTypeRef LLVMInt1TypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMInt32TypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMInt64TypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMHalfTypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMFloatTypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMDoubleTypeInContext(LLVMContextRef C);
LLVMTypeRef LLVMVoidTypeInContext(LLVMContextRef C);

LLVMTypeRef LLVMFunctionType(LLVMTypeRef ReturnType, LLVMTypeRef* ParamTypes,
                             unsigned ParamCount, LLVMBool IsVarArg);

// --- Constants & params ---
LLVMValueRef LLVMConstInt(LLVMTypeRef IntTy, unsigned long long N, LLVMBool SignExtend);
LLVMValueRef LLVMConstReal(LLVMTypeRef Ty, double V);
LLVMValueRef LLVMGetParam(LLVMValueRef Func, unsigned Index);

// Globals (used for the JIT's extern-call slots).
LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef C, unsigned AddressSpace);
LLVMValueRef LLVMConstNull(LLVMTypeRef Ty);
LLVMValueRef LLVMAddGlobal(LLVMModuleRef M, LLVMTypeRef Ty, const char* Name);
void LLVMSetInitializer(LLVMValueRef GlobalVar, LLVMValueRef ConstantVal);

// --- Functions & basic blocks ---
LLVMValueRef LLVMAddFunction(LLVMModuleRef M, const char* Name, LLVMTypeRef FunctionTy);
LLVMBasicBlockRef LLVMAppendBasicBlock(LLVMValueRef Func, const char* Name);
LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef C, LLVMValueRef Func, const char* Name);

// --- Builder ---
LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef C);
void LLVMDisposeBuilder(LLVMBuilderRef B);
void LLVMPositionBuilderAtEnd(LLVMBuilderRef B, LLVMBasicBlockRef Block);

LLVMValueRef LLVMBuildRet(LLVMBuilderRef B, LLVMValueRef V);
LLVMValueRef LLVMBuildRetVoid(LLVMBuilderRef B);
LLVMValueRef LLVMBuildAdd(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildSub(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildMul(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildFAdd(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildFSub(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildFMul(LLVMBuilderRef B, LLVMValueRef L, LLVMValueRef R, const char* Name);
LLVMValueRef LLVMBuildAlloca(LLVMBuilderRef B, LLVMTypeRef Ty, const char* Name);
LLVMValueRef LLVMBuildStore(LLVMBuilderRef B, LLVMValueRef Val, LLVMValueRef Ptr);
LLVMValueRef LLVMBuildLoad2(LLVMBuilderRef B, LLVMTypeRef Ty, LLVMValueRef Ptr, const char* Name);
LLVMValueRef LLVMBuildCall2(LLVMBuilderRef B, LLVMTypeRef FnTy, LLVMValueRef Fn,
                            LLVMValueRef* Args, unsigned NumArgs, const char* Name);

// --- Verification ---
// LLVMVerifierFailureAction: LLVMAbortProcessAction = 0, LLVMReturnStatusAction = 1.
LLVMBool LLVMVerifyModule(LLVMModuleRef M, int VerifierAction, char** OutMessage);

// --- Target initialization ---
// The LLVMInitializeAll* / LLVMInitializeNativeTarget convenience wrappers are
// not exported by this LLVM-C.dll, but the X86-specific entry points are. Since
// the host is x86_64, these are what we need for both JIT and AOT.
void LLVMInitializeX86TargetInfo(void);
void LLVMInitializeX86Target(void);
void LLVMInitializeX86TargetMC(void);
void LLVMInitializeX86AsmPrinter(void);

// --- Targets & target machines (llvm-c/Target.h, TargetMachine.h) ---
typedef struct LLVMOpaqueTarget* LLVMTargetRef;
typedef struct LLVMOpaqueTargetMachine* LLVMTargetMachineRef;
typedef struct LLVMOpaqueMemoryBuffer* LLVMMemoryBufferRef;

typedef enum {
    LLVMCodeGenLevelNone = 0,
    LLVMCodeGenLevelLess = 1,
    LLVMCodeGenLevelDefault = 2,
    LLVMCodeGenLevelAggressive = 3
} LLVMCodeGenOptLevel;

typedef enum {
    LLVMRelocDefault = 0,
    LLVMRelocStatic = 1,
    LLVMRelocPIC = 2,
    LLVMRelocDynamicNoPic = 3
} LLVMRelocMode;

typedef enum {
    LLVMCodeModelDefault = 0,
    LLVMCodeModelJITDefault = 1,
    LLVMCodeModelTiny = 2,
    LLVMCodeModelSmall = 3,
    LLVMCodeModelKernel = 4,
    LLVMCodeModelMedium = 5,
    LLVMCodeModelLarge = 6
} LLVMCodeModel;

typedef enum {
    LLVMAssemblyFile = 0,
    LLVMObjectFile = 1
} LLVMCodeGenFileType;

LLVMBool LLVMGetTargetFromTriple(const char* Triple, LLVMTargetRef* Target,
                                 char** ErrorMessage);
char* LLVMGetDefaultTargetTriple(void);
char* LLVMGetHostCPUName(void);
LLVMTargetMachineRef LLVMCreateTargetMachine(LLVMTargetRef T, const char* Triple,
                                             const char* CPU, const char* Features,
                                             LLVMCodeGenOptLevel Level,
                                             LLVMRelocMode Reloc, LLVMCodeModel CodeModel);
void LLVMDisposeTargetMachine(LLVMTargetMachineRef T);
LLVMBool LLVMTargetMachineEmitToFile(LLVMTargetMachineRef T, LLVMModuleRef M,
                                     const char* Filename, LLVMCodeGenFileType codegen,
                                     char** ErrorMessage);
LLVMBool LLVMTargetMachineEmitToMemoryBuffer(LLVMTargetMachineRef T, LLVMModuleRef M,
                                             LLVMCodeGenFileType codegen,
                                             char** ErrorMessage,
                                             LLVMMemoryBufferRef* OutMemBuf);
void LLVMDisposeMemoryBuffer(LLVMMemoryBufferRef MemBuf);

// --- Execution engine / MCJIT (llvm-c/ExecutionEngine.h) ---
typedef struct LLVMOpaqueExecutionEngine* LLVMExecutionEngineRef;
LLVMBool LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef* OutEE,
                                            LLVMModuleRef M, char** OutError);
void LLVMDisposeExecutionEngine(LLVMExecutionEngineRef EE);
uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef EE, const char* Name);
uint64_t LLVMGetGlobalValueAddress(LLVMExecutionEngineRef EE, const char* Name);

// Symbol resolution for the JIT: binds a declared function/global in `M` to a
// host address. Must be called on the engine before the referencing code is
// compiled (i.e. before LLVMGetFunctionAddress triggers finalization).
LLVMValueRef LLVMGetNamedFunction(LLVMModuleRef M, const char* Name);
void LLVMAddGlobalMapping(LLVMExecutionEngineRef EE, LLVMValueRef Global, void* Addr);
} // extern "C"

namespace strata::llvm_c {
constexpr int kReturnStatusAction = 1;
}
