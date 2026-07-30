#include "strata/Codegen/CodegenBackend.h"

#ifndef STRATA_ENABLE_LLVM
CodegenResult GenerateLlvmIr(const Module* mod)
{
    CodegenResult r = {0};
    return r;
}
#endif
