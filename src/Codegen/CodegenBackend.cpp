#include "strata/Codegen/CodegenBackend.h"

#ifndef STRATA_ENABLE_LLVM
namespace strata
{
CodegenResult GenerateLlvmIr(const Module&)
{
    return {}; // built without LLVM linkage
}
} // namespace strata
#endif
