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
