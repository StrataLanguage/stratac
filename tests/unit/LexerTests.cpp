// Lexer unit tests.
#include "strata/Test.hpp"
#include "Util.hpp"

#include <algorithm>

using namespace strata;
using namespace strata::test_util;

STRATA_TEST(lexer_keywords_and_idents) {
    auto t = lexAll("int float4 MyType inout return");
    auto k = kinds(t);
    STRATA_CHECK(k[0] == TokKind::Kw_int);
    STRATA_CHECK(k[1] == TokKind::Ident);     // float4 is an identifier
    STRATA_CHECK(k[2] == TokKind::Ident);     // MyType
    STRATA_CHECK(k[3] == TokKind::Kw_inout);
    STRATA_CHECK(k[4] == TokKind::Kw_return);
    STRATA_CHECK(k.back() == TokKind::Eof);
}

STRATA_TEST(lexer_line_comment_skipped) {
    auto t = lexAll("// a comment\n42 // trailing\n");
    auto k = kinds(t);
    STRATA_CHECK_EQ(k.size(), (std::size_t)2); // IntLit, Eof
    STRATA_CHECK(k[0] == TokKind::IntLit);
}

STRATA_TEST(lexer_block_comment_nests) {
    auto t = lexAll("/* outer /* inner */ still outer */ 1");
    auto k = kinds(t);
    STRATA_CHECK(k[0] == TokKind::IntLit);
    STRATA_CHECK(k[1] == TokKind::Eof);
}

STRATA_TEST(lexer_integer_literal_kinds) {
    auto t = lexAll("123 0xFF 42u");
    auto k = kinds(t);
    STRATA_CHECK(k[0] == TokKind::IntLit);
    STRATA_CHECK(k[1] == TokKind::IntLit);
    STRATA_CHECK(k[2] == TokKind::IntLit);
    std::string_view src = "123 0xFF 42u";
    STRATA_CHECK_EQ(textOf(src, t[0]), std::string_view("123"));
    STRATA_CHECK_EQ(textOf(src, t[1]), std::string_view("0xFF"));
    STRATA_CHECK_EQ(textOf(src, t[2]), std::string_view("42u"));
}

STRATA_TEST(lexer_float_literals) {
    auto t = lexAll("1.0 1.5e3 .5f 10.0h");
    auto k = kinds(t);
    for (std::size_t i = 0; i < 4; ++i) {
        STRATA_CHECK(k[i] == TokKind::FloatLit);
    }
}

STRATA_TEST(lexer_bool_literals) {
    auto t = lexAll("true false");
    STRATA_CHECK(t[0].is(TokKind::BoolLit));
    STRATA_CHECK(t[1].is(TokKind::BoolLit));
}

STRATA_TEST(lexer_multi_char_operators) {
    auto t = lexAll("<= >= == != && || << >> += ->");
    auto k = kinds(t);
    STRATA_CHECK(k[0] == TokKind::LtEq);
    STRATA_CHECK(k[1] == TokKind::GtEq);
    STRATA_CHECK(k[2] == TokKind::EqEq);
    STRATA_CHECK(k[3] == TokKind::NotEq);
    STRATA_CHECK(k[4] == TokKind::AmpAmp);
    STRATA_CHECK(k[5] == TokKind::PipePipe);
    STRATA_CHECK(k[6] == TokKind::Shl);
    STRATA_CHECK(k[7] == TokKind::Shr);
    STRATA_CHECK(k[8] == TokKind::PlusEq);
    STRATA_CHECK(k[9] == TokKind::Arrow);
}
