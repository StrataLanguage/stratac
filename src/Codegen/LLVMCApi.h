#pragma once


#include <stdint.h>
#include <stddef.h>



#ifdef __cplusplus
extern "C" {
#endif

struct LLVMOpaqueContext;
struct LLVMOpaqueModule;
struct LLVMOpaqueType;
struct LLVMOpaqueValue;
struct LLVMOpaqueBasicBlock;
struct LLVMOpaqueBuilder;
struct LLVMOpaqueTarget;
struct LLVMOpaqueTargetMachine;
struct LLVMOpaqueMemoryBuffer;
struct LLVMOpaqueTargetData;
struct LLVMOpaqueError;
struct LLVMOrcOpaqueThreadSafeContext;
struct LLVMOrcOpaqueThreadSafeModule;
struct LLVMOrcOpaqueJITTargetMachineBuilder;
struct LLVMOrcOpaqueJITDylib;
struct LLVMOrcOpaqueExecutionSession;
struct LLVMOrcOpaqueDefinitionGenerator;
struct LLVMOrcOpaqueSymbolStringPoolEntry;
struct LLVMOrcOpaqueLLJITBuilder;
struct LLVMOrcOpaqueLLJIT;

typedef struct LLVMOpaqueContext* LLVMContextRef;
typedef struct LLVMOpaqueModule* LLVMModuleRef;
typedef struct LLVMOpaqueType* LLVMTypeRef;
typedef struct LLVMOpaqueValue* LLVMValueRef;
typedef struct LLVMOpaqueBasicBlock* LLVMBasicBlockRef;
typedef struct LLVMOpaqueBuilder* LLVMBuilderRef;
typedef struct LLVMOpaqueTarget* LLVMTargetRef;
typedef struct LLVMOpaqueTargetMachine* LLVMTargetMachineRef;
typedef struct LLVMOpaqueMemoryBuffer* LLVMMemoryBufferRef;
typedef struct LLVMOpaqueTargetData* LLVMTargetDataRef;
typedef struct LLVMOpaqueError* LLVMErrorRef;
typedef struct LLVMOrcOpaqueThreadSafeContext* LLVMOrcThreadSafeContextRef;
typedef struct LLVMOrcOpaqueThreadSafeModule* LLVMOrcThreadSafeModuleRef;
typedef struct LLVMOrcOpaqueJITTargetMachineBuilder* LLVMOrcJITTargetMachineBuilderRef;
typedef struct LLVMOrcOpaqueJITDylib* LLVMOrcJITDylibRef;
typedef struct LLVMOrcOpaqueExecutionSession* LLVMOrcExecutionSessionRef;
typedef struct LLVMOrcOpaqueDefinitionGenerator* LLVMOrcDefinitionGeneratorRef;
typedef struct LLVMOrcOpaqueSymbolStringPoolEntry* LLVMOrcSymbolStringPoolEntryRef;
typedef struct LLVMOrcOpaqueLLJITBuilder* LLVMOrcLLJITBuilderRef;
typedef struct LLVMOrcOpaqueLLJIT* LLVMOrcLLJITRef;
typedef uint64_t LLVMOrcExecutorAddress;


typedef enum {
  LLVMVoidTypeKind = 0,     /**< type with no size */
  LLVMHalfTypeKind = 1,     /**< 16 bit floating point type */
  LLVMFloatTypeKind = 2,    /**< 32 bit floating point type */
  LLVMDoubleTypeKind = 3,   /**< 64 bit floating point type */
  LLVMX86_FP80TypeKind = 4, /**< 80 bit floating point type (X87) */
  LLVMFP128TypeKind = 5, /**< 128 bit floating point type (112-bit mantissa)*/
  LLVMPPC_FP128TypeKind = 6, /**< 128 bit floating point type (two 64-bits) */
  LLVMLabelTypeKind = 7,     /**< Labels */
  LLVMIntegerTypeKind = 8,   /**< Arbitrary bit width integers */
  LLVMFunctionTypeKind = 9,  /**< Functions */
  LLVMStructTypeKind = 10,   /**< Structures */
  LLVMArrayTypeKind = 11,    /**< Arrays */
  LLVMPointerTypeKind = 12,  /**< Pointers */
  LLVMVectorTypeKind = 13,   /**< Fixed width SIMD vector type */
  LLVMMetadataTypeKind = 14, /**< Metadata */
                             /* 15 previously used by LLVMX86_MMXTypeKind */
  LLVMTokenTypeKind = 16,    /**< Tokens */
  LLVMScalableVectorTypeKind = 17, /**< Scalable SIMD vector type */
  LLVMBFloatTypeKind = 18,         /**< 16 bit brain floating point type */
  LLVMX86_AMXTypeKind = 19,        /**< X86 AMX */
  LLVMTargetExtTypeKind = 20,      /**< Target extension type */
} LLVMTypeKind;

typedef int LLVMBool;

void LLVMGetVersion(unsigned* major, unsigned* minor, unsigned* patch);

LLVMContextRef LLVMContextCreate(void);
void LLVMContextDispose(LLVMContextRef c);

LLVMModuleRef LLVMModuleCreateWithNameInContext(const char* moduleId, LLVMContextRef c);
void LLVMDisposeModule(LLVMModuleRef m);
void LLVMDumpModule(LLVMModuleRef m);
char* LLVMPrintModuleToString(LLVMModuleRef m);
void LLVMDisposeMessage(char* message);

LLVMTypeRef LLVMInt1TypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMInt8TypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMInt16TypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMInt32TypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMInt64TypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMHalfTypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMFloatTypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMDoubleTypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMVoidTypeInContext(LLVMContextRef c);
LLVMTypeRef LLVMVectorType(LLVMTypeRef elementType, unsigned elementCount);
LLVMTypeRef LLVMStructCreateNamed(LLVMContextRef c, const char* name);
LLVMBool LLVMStructSetBody(LLVMTypeRef structTy, LLVMTypeRef* elementTypes, unsigned elementCount, LLVMBool packed);

LLVMTypeRef LLVMFunctionType(LLVMTypeRef returnType, LLVMTypeRef* paramTypes, unsigned paramCount, LLVMBool isVarArg);

LLVMValueRef LLVMConstInt(LLVMTypeRef intTy, unsigned long long n, LLVMBool signExtend);
LLVMValueRef LLVMConstReal(LLVMTypeRef ty, double v);
LLVMValueRef LLVMGetUndef(LLVMTypeRef ty);
LLVMValueRef LLVMGetPoison(LLVMTypeRef ty);
LLVMTypeRef LLVMStructTypeInContext(LLVMContextRef c, LLVMTypeRef* elementTypes, unsigned elementCount, LLVMBool packed);
LLVMTypeRef LLVMArrayType(LLVMTypeRef elementType, unsigned elementCount);
LLVMValueRef LLVMGetParam(LLVMValueRef func, unsigned index);

LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef c, unsigned addressSpace);
LLVMValueRef LLVMConstVector(LLVMValueRef* maskv, int ncomp);
LLVMValueRef LLVMConstNull(LLVMTypeRef ty);
LLVMValueRef LLVMConstGEP2(LLVMTypeRef ty, LLVMValueRef pointer, LLVMValueRef* indices, unsigned numIndices);
LLVMValueRef LLVMConstPtrToInt(LLVMValueRef constantVal, LLVMTypeRef toType);
LLVMValueRef LLVMConstBitCast(LLVMValueRef constantVal, LLVMTypeRef toType);
LLVMValueRef LLVMConstNamedStruct(LLVMTypeRef structTy, LLVMValueRef* constantMembers, unsigned memberCount);
LLVMValueRef LLVMConstArray(LLVMTypeRef elementTy, LLVMValueRef* constantVals, unsigned length);
LLVMValueRef LLVMBuildPtrToInt(LLVMBuilderRef b, LLVMValueRef val, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMAddGlobal(LLVMModuleRef m, LLVMTypeRef ty, const char* name);
void LLVMSetInitializer(LLVMValueRef globalVar, LLVMValueRef constantVal);
void LLVMSetLinkage(LLVMValueRef global, int linkage);
void LLVMSetUnnamedAddr(LLVMValueRef global, LLVMBool hasUnnamedAddr);
void LLVMSetGlobalConstant(LLVMValueRef global, LLVMBool isConstant);
LLVMValueRef LLVMConstStringInContext(LLVMContextRef c, const char* str, unsigned length, LLVMBool dontNullTerminate);
LLVMTypeRef LLVMTypeOf(LLVMValueRef val);
LLVMTypeKind LLVMGetTypeKind(LLVMTypeRef Ty);

LLVMValueRef LLVMAddFunction(LLVMModuleRef m, const char* name, LLVMTypeRef functionTy);
LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef c, LLVMValueRef func, const char* name);

LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef c);
void LLVMDisposeBuilder(LLVMBuilderRef b);
void LLVMPositionBuilderAtEnd(LLVMBuilderRef b, LLVMBasicBlockRef block);

LLVMValueRef LLVMBuildRet(LLVMBuilderRef b, LLVMValueRef v);
LLVMValueRef LLVMBuildRetVoid(LLVMBuilderRef b);
LLVMValueRef LLVMBuildBr(LLVMBuilderRef b, LLVMBasicBlockRef dest);
LLVMValueRef LLVMBuildCondBr(LLVMBuilderRef b, LLVMValueRef If, LLVMBasicBlockRef T, LLVMBasicBlockRef E);
LLVMValueRef LLVMBuildUnreachable(LLVMBuilderRef b);
LLVMValueRef LLVMBuildSelect(LLVMBuilderRef b, LLVMValueRef If, LLVMValueRef Then, LLVMValueRef Else, const char* name);
LLVMValueRef LLVMBuildAdd(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildSub(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildMul(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildFAdd(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildFSub(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildFMul(LLVMBuilderRef b, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildAlloca(LLVMBuilderRef b, LLVMTypeRef ty, const char* name);
LLVMValueRef LLVMBuildStore(LLVMBuilderRef b, LLVMValueRef val, LLVMValueRef ptr);
LLVMValueRef LLVMBuildLoad2(LLVMBuilderRef b, LLVMTypeRef ty, LLVMValueRef ptr, const char* name);
LLVMValueRef LLVMBuildCall2(LLVMBuilderRef b, LLVMTypeRef fnTy, LLVMValueRef fn, LLVMValueRef* args, unsigned numArgs, const char* name);
LLVMValueRef LLVMBuildBitCast(LLVMBuilderRef b, LLVMValueRef val, LLVMTypeRef destTy, const char *name);

typedef enum {
    LLVMIntEQ = 32, LLVMIntNE, LLVMIntUGT, LLVMIntUGE, LLVMIntULT, LLVMIntULE,
    LLVMIntSGT, LLVMIntSGE, LLVMIntSLT, LLVMIntSLE
} LLVMIntPredicate;

typedef enum {
    LLVMRealPredicateFalse = 0, LLVMRealOEQ = 1, LLVMRealOGT = 2, LLVMRealOGE = 3,
    LLVMRealOLT = 4, LLVMRealOLE = 5, LLVMRealONE = 6, LLVMRealORD = 7,
    LLVMRealUNO = 8, LLVMRealUEQ = 9, LLVMRealUGT = 10, LLVMRealUGE = 11,
    LLVMRealULT = 12, LLVMRealULE = 13, LLVMRealUNE = 14, LLVMRealPredicateTrue = 15
} LLVMRealPredicate;


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
LLVMValueRef LLVMBuildICmp(LLVMBuilderRef b, LLVMIntPredicate pred, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildFCmp(LLVMBuilderRef b, LLVMRealPredicate pred, LLVMValueRef l, LLVMValueRef r, const char* name);
LLVMValueRef LLVMBuildSIToFP(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildUIToFP(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildFPToSI(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildFPToUI(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildIntCast2(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, LLVMBool isSigned, const char* name);
LLVMValueRef LLVMBuildSExt(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildZExt(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);
LLVMValueRef LLVMBuildFPExt(LLVMBuilderRef b, LLVMValueRef v, LLVMTypeRef destTy, const char* name);

LLVMValueRef LLVMBuildInsertValue(LLVMBuilderRef b, LLVMValueRef agg, LLVMValueRef val, unsigned index, const char* name);

LLVMValueRef LLVMBuildShuffleVector(LLVMBuilderRef b, LLVMValueRef v1, LLVMValueRef v2, LLVMValueRef mask, const char *name);
LLVMValueRef LLVMBuildInsertElement(LLVMBuilderRef b, LLVMValueRef v, LLVMValueRef eltVal, LLVMValueRef index, const char *name);
LLVMValueRef LLVMBuildExtractValue(LLVMBuilderRef b, LLVMValueRef agg, unsigned index, const char* name);
LLVMValueRef LLVMBuildExtractElement(LLVMBuilderRef b, LLVMValueRef vecVal, LLVMValueRef index, const char *name);
LLVMValueRef LLVMBuildGEP2(LLVMBuilderRef b, LLVMTypeRef ty, LLVMValueRef pointer, LLVMValueRef* indices, unsigned numIndices, const char* name);
LLVMValueRef LLVMBuildPhi(LLVMBuilderRef b, LLVMTypeRef ty, const char* name);
void LLVMAddIncoming(LLVMValueRef phiNode, LLVMValueRef* incomingValues, LLVMBasicBlockRef* incomingBlocks, unsigned count);
LLVMBasicBlockRef LLVMGetInsertBlock(LLVMBuilderRef b);
void LLVMPositionBuilderBefore(LLVMBuilderRef b, LLVMValueRef inst);
LLVMValueRef LLVMGetFirstInstruction(LLVMBasicBlockRef bb);
LLVMValueRef LLVMGetBasicBlockTerminator(LLVMBasicBlockRef bb);

LLVMBool LLVMVerifyModule(LLVMModuleRef m, int verifierAction, char** outMessage);

void LLVMInitializeX86TargetInfo(void);
void LLVMInitializeX86Target(void);
void LLVMInitializeX86TargetMC(void);
void LLVMInitializeX86AsmPrinter(void);
void LLVMInitializeAArch64TargetInfo(void);
void LLVMInitializeAArch64Target(void);
void LLVMInitializeAArch64TargetMC(void);
void LLVMInitializeAArch64AsmPrinter(void);

typedef enum {
    LLVMCodeGenLevelNone = 0, LLVMCodeGenLevelLess = 1,
    LLVMCodeGenLevelDefault = 2, LLVMCodeGenLevelAggressive = 3
} LLVMCodeGenOptLevel;

typedef enum {
    LLVMRelocDefault = 0, LLVMRelocStatic = 1,
    LLVMRelocPIC = 2, LLVMRelocDynamicNoPic = 3
} LLVMRelocMode;

typedef enum {
    LLVMCodeModelDefault = 0, LLVMCodeModelJITDefault = 1, LLVMCodeModelTiny = 2,
    LLVMCodeModelSmall = 3, LLVMCodeModelKernel = 4,
    LLVMCodeModelMedium = 5, LLVMCodeModelLarge = 6
} LLVMCodeModel;

typedef enum {
    LLVMAssemblyFile = 0, LLVMObjectFile = 1
} LLVMCodeGenFileType;

LLVMBool LLVMGetTargetFromTriple(const char* triple, LLVMTargetRef* target, char** errorMessage);
char* LLVMGetDefaultTargetTriple(void);
char* LLVMGetHostCPUName(void);
LLVMTargetMachineRef LLVMCreateTargetMachine(LLVMTargetRef t, const char* triple, const char* cpu,
                                             const char* features, LLVMCodeGenOptLevel level,
                                             LLVMRelocMode reloc, LLVMCodeModel codeModel);
void LLVMDisposeTargetMachine(LLVMTargetMachineRef t);
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

LLVMValueRef LLVMGetNamedFunction(LLVMModuleRef m, const char* name);

/* --- llvm-c/Error.h --- */
char* LLVMGetErrorMessage(LLVMErrorRef err);
void LLVMDisposeErrorMessage(char* errMsg);
void LLVMConsumeError(LLVMErrorRef err);

/* --- llvm-c/Orc.h (ORCv2) --- */
typedef enum {
    LLVMJITSymbolGenericFlagsNone = 0,
    LLVMJITSymbolGenericFlagsExported = 1U << 0,
    LLVMJITSymbolGenericFlagsWeak = 1U << 1,
    LLVMJITSymbolGenericFlagsCallable = 1U << 2,
    LLVMJITSymbolGenericFlagsMaterializationSideEffectsOnly = 1U << 3
} LLVMJITSymbolGenericFlags;

typedef struct {
    uint8_t GenericFlags;
    uint8_t TargetFlags;
} LLVMJITSymbolFlags;

typedef struct {
    LLVMOrcExecutorAddress Address;
    LLVMJITSymbolFlags Flags;
} LLVMJITEvaluatedSymbol;

typedef struct {
    LLVMOrcSymbolStringPoolEntryRef Name;
    LLVMJITEvaluatedSymbol Sym;
} LLVMOrcCSymbolMapPair;

typedef LLVMOrcCSymbolMapPair* LLVMOrcCSymbolMapPairs;

typedef int (*LLVMOrcSymbolPredicate)(void* ctx, LLVMOrcSymbolStringPoolEntryRef sym);

typedef struct LLVMOrcOpaqueMaterializationUnit* LLVMOrcMaterializationUnitRef;

LLVMOrcThreadSafeContextRef LLVMOrcCreateNewThreadSafeContextFromLLVMContext(LLVMContextRef ctx);
void LLVMOrcDisposeThreadSafeContext(LLVMOrcThreadSafeContextRef tsCtx);
LLVMOrcThreadSafeModuleRef LLVMOrcCreateNewThreadSafeModule(LLVMModuleRef m, LLVMOrcThreadSafeContextRef tsCtx);
void LLVMOrcDisposeThreadSafeModule(LLVMOrcThreadSafeModuleRef tsm);

LLVMErrorRef LLVMOrcJITTargetMachineBuilderDetectHost(LLVMOrcJITTargetMachineBuilderRef* outResult);
void LLVMOrcDisposeJITTargetMachineBuilder(LLVMOrcJITTargetMachineBuilderRef jtmb);

LLVMErrorRef LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
    LLVMOrcDefinitionGeneratorRef* outResult, char globalPrefix,
    LLVMOrcSymbolPredicate filter, void* filterCtx);
void LLVMOrcJITDylibAddGenerator(LLVMOrcJITDylibRef jd, LLVMOrcDefinitionGeneratorRef dg);

LLVMOrcMaterializationUnitRef LLVMOrcAbsoluteSymbols(LLVMOrcCSymbolMapPairs syms, size_t numPairs);
LLVMErrorRef LLVMOrcJITDylibDefine(LLVMOrcJITDylibRef jd, LLVMOrcMaterializationUnitRef mu);

/* --- llvm-c/LLJIT.h (ORCv2 LLJIT) --- */
LLVMOrcLLJITBuilderRef LLVMOrcCreateLLJITBuilder(void);
void LLVMOrcDisposeLLJITBuilder(LLVMOrcLLJITBuilderRef builder);
void LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(LLVMOrcLLJITBuilderRef builder, LLVMOrcJITTargetMachineBuilderRef jtmb);

LLVMErrorRef LLVMOrcCreateLLJIT(LLVMOrcLLJITRef* outResult, LLVMOrcLLJITBuilderRef builder);
LLVMErrorRef LLVMOrcDisposeLLJIT(LLVMOrcLLJITRef j);

LLVMOrcJITDylibRef LLVMOrcLLJITGetMainJITDylib(LLVMOrcLLJITRef j);
char LLVMOrcLLJITGetGlobalPrefix(LLVMOrcLLJITRef j);
LLVMOrcSymbolStringPoolEntryRef LLVMOrcLLJITMangleAndIntern(LLVMOrcLLJITRef j, const char* unmangledName);

LLVMErrorRef LLVMOrcLLJITAddLLVMIRModule(LLVMOrcLLJITRef j, LLVMOrcJITDylibRef jd, LLVMOrcThreadSafeModuleRef tsm);
LLVMErrorRef LLVMOrcLLJITLookup(LLVMOrcLLJITRef j, LLVMOrcExecutorAddress* outResult, const char* name);

#define kReturnStatusAction 1

#define LLVMAppendingLinkage 7
#define LLVMInternalLinkage 8
#define LLVMPrivateLinkage 9

#ifdef __cplusplus
}
#endif
