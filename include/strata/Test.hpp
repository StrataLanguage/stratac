// Strata compiler: a tiny self-contained test framework.
//
// We avoid pulling in a third-party unit-test dependency by shipping this
// minimal header. It supports TEST() blocks, CHECK/CHECK_EQ/REQUIRE, and a
// single test runner. Good enough for bootstrap; replaceable later.
//
// Usage:
//   // in a test .cpp:
//   STRATA_TEST(lexer_tokenizes_int) {
//       STRATA_CHECK(1 + 1 == 2);
//       STRATA_CHECK_EQ(2 * 3, 6);
//   }
//   // exactly one TU defines the runner via STRATA_TEST_MAIN before including:
//   #define STRATA_TEST_MAIN
//   #include "strata/Test.hpp"
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace strata::test
{

using TestFn = void (*)();

struct Registration
{
    const char* name;
    TestFn fn;
};

inline std::vector<Registration>& Registry()
{
    static std::vector<Registration> r;
    return r;
}

inline int& FailureCount()
{
    static int n = 0;
    return n;
}

inline void RegisterTest(const char* name, TestFn fn)
{
    Registry().push_back({.name = name, .fn = fn});
}

inline void FailAt(const char* expr, const char* file, int line)
{
    ++FailureCount();
    std::printf("  FAIL %s(%d): %s\n", file, line, expr);
}

template <typename T> std::string ToStr(const T& v)
{
    if constexpr (std::is_convertible_v<T, std::string_view>)
    {
        return std::string(v);
    }
    else
    {
        return std::to_string(v);
    }
}

template <typename A, typename B>
inline void FailEqAt(const char* ea, const char* eb, const A& a, const B& b, const char* file, int line)
{
    ++FailureCount();
    std::string sa = ToStr(a);
    std::string sb = ToStr(b);
    std::printf("  FAIL %s(%d): %s != %s  (got %s vs %s)\n", file, line, ea, eb, sa.c_str(), sb.c_str());
}

inline int RunAll()
{
    int total = 0;
    for (const auto& reg : Registry())
    {
        int before = FailureCount();
        std::printf("[ RUN      ] %s\n", reg.name);
        std::fflush(stdout);
        reg.fn();
        std::fflush(stdout);
        int failed = FailureCount() - before;
        if (failed == 0)
        {
            std::printf("[       OK ] %s\n", reg.name);
        }
        else
        {
            std::printf("[  FAILED  ] %s (%d assertion(s))\n", reg.name, failed);
        }
        std::fflush(stdout);
        ++total;
    }
    int fails = FailureCount();
    std::printf("== %d test(s), %d failure(s) ==\n", total, fails);
    return fails;
}

} // namespace strata::test

#define STRATA_TEST(name)                                                                                              \
    static void name##_impl();                                                                                         \
    struct name##_Registrar                                                                                            \
    {                                                                                                                  \
        name##_Registrar()                                                                                             \
        {                                                                                                              \
            ::strata::test::RegisterTest(#name, &name##_impl);                                                         \
        }                                                                                                              \
    } name##_registrar_inst;                                                                                           \
    static void name##_impl()

#define STRATA_CHECK(cond)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond)) ::strata::test::FailAt(#cond, __FILE__, __LINE__);                                                \
    } while (0)

#define STRATA_CHECK_EQ(a, b)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _sa = (a);                                                                                                \
        auto _sb = (b);                                                                                                \
        if (!(_sa == _sb)) ::strata::test::FailEqAt(#a, #b, _sa, _sb, __FILE__, __LINE__);                             \
    } while (0)

#ifdef STRATA_TEST_MAIN
int main()
{
    return ::strata::test::RunAll() == 0 ? 0 : 1;
}
#endif
