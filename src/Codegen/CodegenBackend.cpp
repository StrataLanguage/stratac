// Strata compiler: back-end factory bookkeeping.
//
// createTextBackend() is always available (it emits text IR and needs no LLVM
// linkage). createLLVMBackend() is defined here as a null stub unless the build
// links LLVM; the real implementation in LLVMCBackend.cpp overrides it when
// STRATA_ENABLE_LLVM is defined by CMake.
#include "strata/Codegen/CodegenBackend.h"

#ifndef STRATA_ENABLE_LLVM
namespace strata
{
std::unique_ptr<CodegenBackend> CreateLlvmBackend()
{
    return nullptr; // built without LLVM linkage
}
} // namespace strata
#endif
