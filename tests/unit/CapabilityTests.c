#include "Test.h"
#include "strata/strata.h"

#include <string.h>

STRATA_TEST(build_capabilities_match_configuration)
{
    unsigned capabilities = strataCapabilities();
    STRATA_CHECK((capabilities & STRATA_CAP_C_OUTPUT) != 0);
#if STRATA_TEST_HAS_TCC
    STRATA_CHECK((capabilities & STRATA_CAP_TCC_JIT) != 0);
#else
    STRATA_CHECK((capabilities & STRATA_CAP_TCC_JIT) == 0);
#endif
#if STRATA_TEST_HAS_LLVM
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_IR) != 0);
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_AOT) != 0);
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_JIT) != 0);
#else
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_IR) == 0);
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_AOT) == 0);
    STRATA_CHECK((capabilities & STRATA_CAP_LLVM_JIT) == 0);
    STRATA_CHECK(strcmp(strataLLVMVersion(), "disabled") == 0);
#endif
}

#if !STRATA_TEST_HAS_LLVM
STRATA_TEST(no_llvm_apis_fail_explicitly)
{
    StrataCompiler* compiler = strataCompilerCreate();
    StrataResult result = strataCompileString(
        compiler, "int entry() { return 1; }", "no_llvm", STRATA_EMIT_LLVM_IR, 0);
    STRATA_CHECK(!result.ok);
    STRATA_CHECK(strstr(result.diagnostics, "LLVM backend not built") != NULL);
    strataResultFree(&result);

    const char* error = NULL;
    STRATA_CHECK(!strataCompileToObject(compiler, "ignored.strata", "ignored.o", 0, &error));
    STRATA_CHECK(error != NULL);
    STRATA_CHECK(strstr(error, "LLVM backend not built") != NULL);
    strataFree((char*)error);
    strataCompilerDestroy(compiler);
}
#endif
