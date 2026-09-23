#include "Test.h"
#include "Util.h"

STRATA_TEST(lexer_keywords_and_idents)
{
    TokenList t = LexAll("int float4 MyType ref return");
    TokKind* k = Kinds(t);
    STRATA_CHECK(k[0] == TokKwInt);
    STRATA_CHECK(k[1] == TokKwFloat4);
    STRATA_CHECK(k[2] == TokIdent);
    STRATA_CHECK(k[3] == TokKwRef);
    STRATA_CHECK(k[4] == TokKwReturn);
    STRATA_CHECK(k[t.count - 1] == TokEof);
    free(k);
    free(t.items);
}

STRATA_TEST(lexer_line_comment_skipped)
{
    TokenList t = LexAll("// a comment\n42 // trailing\n");
    TokKind* k = Kinds(t);
    STRATA_CHECK_EQ((long)t.count, 2);
    STRATA_CHECK(k[0] == TokIntLit);
    free(k);
    free(t.items);
}

STRATA_TEST(lexer_block_comment_nests)
{
    TokenList t = LexAll("/* outer /* inner */ still outer */ 1");
    TokKind* k = Kinds(t);
    STRATA_CHECK(k[0] == TokIntLit);
    STRATA_CHECK(k[1] == TokEof);
    free(k);
    free(t.items);
}

STRATA_TEST(lexer_integer_literal_kinds)
{
    const char* src = "123 0xFF 42u";
    TokenList t = LexAll(src);
    TokKind* k = Kinds(t);
    STRATA_CHECK(k[0] == TokIntLit);
    STRATA_CHECK(k[1] == TokIntLit);
    STRATA_CHECK(k[2] == TokIntLit);
    STRATA_CHECK(StrEqC(TextOf(src, t.items[0]), "123"));
    STRATA_CHECK(StrEqC(TextOf(src, t.items[1]), "0xFF"));
    STRATA_CHECK(StrEqC(TextOf(src, t.items[2]), "42u"));
    free(k);
    free(t.items);
}

STRATA_TEST(lexer_float_literals)
{
    TokenList t = LexAll("1.0 1.5e3 .5f");
    TokKind* k = Kinds(t);
    for (size_t i = 0; i < 3; ++i)
    {
        STRATA_CHECK(k[i] == TokFloatLit);
    }
    free(k);
    free(t.items);
}

STRATA_TEST(lexer_bool_literals)
{
    TokenList t = LexAll("true false");
    STRATA_CHECK(t.items[0].kind == TokBoolLit);
    STRATA_CHECK(t.items[1].kind == TokBoolLit);
    free(t.items);
}

STRATA_TEST(lexer_multi_char_operators)
{
    TokenList t = LexAll("<= >= == != && || << >> += ->");
    TokKind* k = Kinds(t);
    STRATA_CHECK(k[0] == TokLtEq);
    STRATA_CHECK(k[1] == TokGtEq);
    STRATA_CHECK(k[2] == TokEqEq);
    STRATA_CHECK(k[3] == TokNotEq);
    STRATA_CHECK(k[4] == TokAmpAmp);
    STRATA_CHECK(k[5] == TokPipePipe);
    STRATA_CHECK(k[6] == TokShl);
    STRATA_CHECK(k[7] == TokShr);
    STRATA_CHECK(k[8] == TokPlusEq);
    STRATA_CHECK(k[9] == TokArrow);
    free(k);
    free(t.items);
}

/* Lexes `src` to EOF and returns how many errors the lexer reported. */
static uint32_t LexErrorCount(const char* src)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Lexer lex;
    LexerInit(&lex, src, strlen(src), &diag, 0);

    while (LexerNextToken(&lex).kind != TokEof)
    {
    }

    uint32_t errors = DiagErrorCount(&diag);
    DiagnosticEngineFree(&diag);
    return errors;
}

STRATA_TEST(lexer_token_longer_than_64k_keeps_full_length)
{
    /* SourceRange.length used to be 16 bits: a 70,000-byte token wrapped to 4,464. */
    size_t n = 70000;
    char* src = (char*)malloc(n + 3);
    src[0] = '"';
    memset(src + 1, 'a', n);
    src[n + 1] = '"';
    src[n + 2] = '\0';

    TokenList t = LexAll(src);
    STRATA_CHECK(t.items[0].kind == TokStrLit);
    STRATA_CHECK_EQ((long)t.items[0].range.length, (long)(n + 2));
    STRATA_CHECK(t.items[1].kind == TokEof);

    free(t.items);
    free(src);
}

STRATA_TEST(lexer_malformed_number_literals_are_diagnosed)
{
    STRATA_CHECK_EQ(LexErrorCount("0x"), 1);
    STRATA_CHECK_EQ(LexErrorCount("0xu"), 1);
    STRATA_CHECK_EQ(LexErrorCount("1e"), 1);
    STRATA_CHECK_EQ(LexErrorCount("1e+"), 1);
    STRATA_CHECK_EQ(LexErrorCount("2.5E-"), 1);
    STRATA_CHECK_EQ(LexErrorCount("1.5u"), 1);
    STRATA_CHECK_EQ(LexErrorCount("1e5u"), 1);
}

STRATA_TEST(lexer_well_formed_number_literals_are_clean)
{
    STRATA_CHECK_EQ(LexErrorCount("0 010 0x1F 0XffU 42u 1.0 .5 1e5 1E+5 2.5e-3 1.5f 3f"), 0);
}
