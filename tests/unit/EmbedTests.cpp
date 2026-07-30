// Public C embedding API round-trip tests.
#include "strata/Test.hpp"
#include "strata/strata.h"

#include <cstring>

STRATA_TEST(embed_compile_string_ok)
{
    StrataCompiler* c = strataCompilerCreate();
    STRATA_CHECK(c != nullptr);
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_LLVM_IR);
    STRATA_CHECK_EQ(r.ok, 1);
    STRATA_CHECK_EQ(r.error_count, (unsigned)0);
    STRATA_CHECK(r.output != nullptr);
    STRATA_CHECK(std::strstr(r.output, "define") != nullptr);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_compile_string_reports_errors)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f( { }", "m", STRATA_EMIT_LLVM_IR);
    STRATA_CHECK_EQ(r.ok, 0);
    STRATA_CHECK(r.error_count > 0);
    STRATA_CHECK(r.diagnostics != nullptr);
    STRATA_CHECK(std::strstr(r.diagnostics, "error") != nullptr);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_ast_emit)
{
    StrataCompiler* c = strataCompilerCreate();
    StrataResult r = strataCompileString(c, "int f() { return 1; }", "m", STRATA_EMIT_AST);
    STRATA_CHECK_EQ(r.ok, 1);
    STRATA_CHECK(std::strstr(r.output, "fn int f") != nullptr);
    strataResultFree(&r);
    strataCompilerDestroy(c);
}

STRATA_TEST(embed_version_is_reported)
{
    const char* v = strataLLVMVersion();
    STRATA_CHECK(v != nullptr);
    STRATA_CHECK(v[0] != '\0');
}
