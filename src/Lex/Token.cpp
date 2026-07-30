#include "strata/Lex/Token.h"

#include <array>

namespace strata {

namespace {

struct Spelling { TokKind kind; std::string_view text; };

// Order matters only for readability; lookup is linear over a tiny table.
constexpr std::array<Spelling, 26> kKeywords = {{
    {TokKind::Kw_void,      "void"},
    {TokKind::Kw_bool,      "bool"},
    {TokKind::Kw_int,       "int"},
    {TokKind::Kw_uint,      "uint"},
    {TokKind::Kw_half,      "half"},
    {TokKind::Kw_float,     "float"},
    {TokKind::Kw_double,    "double"},
    {TokKind::Kw_string,    "string"},
    {TokKind::Kw_in,        "in"},
    {TokKind::Kw_out,       "out"},
    {TokKind::Kw_inout,     "inout"},
    {TokKind::Kw_const,     "const"},
    {TokKind::Kw_static,    "static"},
    {TokKind::Kw_return,    "return"},
    {TokKind::Kw_if,        "if"},
    {TokKind::Kw_else,      "else"},
    {TokKind::Kw_while,     "while"},
    {TokKind::Kw_for,       "for"},
    {TokKind::Kw_break,     "break"},
    {TokKind::Kw_continue,  "continue"},
    {TokKind::Kw_true,      "true"},
    {TokKind::Kw_false,     "false"},
    {TokKind::Kw_struct,    "struct"},
    {TokKind::Kw_namespace, "namespace"},
}};

constexpr std::array<Spelling, 38> kPunct = {{
    {TokKind::LParen,    "("},
    {TokKind::RParen,    ")"},
    {TokKind::LBrace,    "{"},
    {TokKind::RBrace,    "}"},
    {TokKind::LBracket,  "["},
    {TokKind::RBracket,  "]"},
    {TokKind::Comma,     ","},
    {TokKind::Semicolon, ";"},
    {TokKind::Colon,     ":"},
    {TokKind::Dot,       "."},
    {TokKind::Arrow,     "->"},
    {TokKind::Assign,    "="},
    {TokKind::Plus,      "+"},
    {TokKind::Minus,     "-"},
    {TokKind::Star,      "*"},
    {TokKind::Slash,     "/"},
    {TokKind::Percent,   "%"},
    {TokKind::Amp,       "&"},
    {TokKind::Pipe,      "|"},
    {TokKind::Caret,     "^"},
    {TokKind::Tilde,     "~"},
    {TokKind::Bang,      "!"},
    {TokKind::Lt,        "<"},
    {TokKind::Gt,        ">"},
    {TokKind::LtEq,      "<="},
    {TokKind::GtEq,      ">="},
    {TokKind::EqEq,      "=="},
    {TokKind::NotEq,     "!="},
    {TokKind::AmpAmp,    "&&"},
    {TokKind::PipePipe,  "||"},
    {TokKind::Shl,       "<<"},
    {TokKind::Shr,       ">>"},
    {TokKind::PlusEq,    "+="},
    {TokKind::MinusEq,   "-="},
    {TokKind::StarEq,    "*="},
    {TokKind::SlashEq,   "/="},
    {TokKind::PercentEq, "%="},
}};

} // namespace

TokKind classifyKeyword(std::string_view ident) noexcept {
    for (auto& kw : kKeywords) {
        if (ident == kw.text) return kw.kind;
    }
    return TokKind::Ident;
}

std::string_view tokSpelling(TokKind k) noexcept {
    for (auto& kw : kKeywords) if (kw.kind == k) return kw.text;
    for (auto& p : kPunct)    if (p.kind == k) return p.text;
    switch (k) {
        case TokKind::Eof:   return "<eof>";
        case TokKind::Ident: return "<ident>";
        default:             return "<tok>";
    }
}

std::string_view tokName(TokKind k) noexcept {
    switch (k) {
        case TokKind::Eof:      return "end of input";
        case TokKind::Unknown:  return "unknown token";
        case TokKind::Ident:    return "identifier";
        case TokKind::IntLit:   return "integer literal";
        case TokKind::FloatLit: return "float literal";
        case TokKind::BoolLit:  return "bool literal";
        default: break;
    }
    // Keyword?
    for (auto& kw : kKeywords) {
        if (kw.kind == k) return kw.text; // "return", "int", ...
    }
    auto sp = tokSpelling(k);
    // Punctuation: wrap in quotes.
    return sp;
}

} // namespace strata
