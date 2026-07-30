#include "strata/Lex/Token.h"

#include <array>

namespace strata
{

namespace
{

struct Spelling
{
    TokKind kind;
    std::string_view text;
};

// Order matters only for readability; lookup is linear over a tiny table.
constexpr Spelling kKeywords[] = {
    { TokKind::KwVoid, "void" },
    { TokKind::KwBool, "bool" },
    { TokKind::KwInt, "int" },
    { TokKind::KwUint, "uint" },
    { TokKind::KwFloat, "float" },
    { TokKind::KwDouble, "double" },
    { TokKind::KwString, "string" },
    { TokKind::KwIn, "in" },
    { TokKind::KwOut, "out" },
    { TokKind::KwInout, "inout" },
    { TokKind::KwConst, "const" },
    { TokKind::KwStatic, "static" },
    { TokKind::KwExtern, "extern" },
    { TokKind::KwHandle, "handle" },
    { TokKind::KwReturn, "return" },
    { TokKind::KwIf, "if" },
    { TokKind::KwElse, "else" },
    { TokKind::KwWhile, "while" },
    { TokKind::KwFor, "for" },
    { TokKind::KwBreak, "break" },
    { TokKind::KwContinue, "continue" },
    { TokKind::KwTrue, "true" },
    { TokKind::KwFalse, "false" },
    { TokKind::KwStruct, "struct" },
    { TokKind::KwNamespace, "namespace" }
};

constexpr Spelling kPunct[] = {
    { TokKind::LParen, "(" },
    { TokKind::RParen, ")" },
    { TokKind::LBrace, "{" },
    { TokKind::RBrace, "}" },
    { TokKind::LBracket, "[" },
    { TokKind::RBracket, "]" },
    { TokKind::Comma, "," },
    { TokKind::Semicolon, ";" },
    { TokKind::Colon, ":" },
    { TokKind::Dot, "." },
    { TokKind::Arrow, "->" },
    { TokKind::Assign, "=" },
    { TokKind::Plus, "+" },
    { TokKind::Minus, "-" },
    { TokKind::Star, "*" },
    { TokKind::Slash, "/" },
    { TokKind::Percent, "%" },
    { TokKind::Amp, "&" },
    { TokKind::Pipe, "|" },
    { TokKind::Caret, "^" },
    { TokKind::Tilde, "~" },
    { TokKind::Bang, "!" },
    { TokKind::Lt, "<" },
    { TokKind::Gt, ">" },
    { TokKind::LtEq, "<=" },
    { TokKind::GtEq, ">=" },
    { TokKind::EqEq, "==" },
    { TokKind::NotEq, "!=" },
    { TokKind::AmpAmp, "&&" },
    { TokKind::PipePipe, "||" },
    { TokKind::Shl, "<<" },
    { TokKind::Shr, ">>" },
    { TokKind::PlusEq, "+=" },
    { TokKind::MinusEq, "-=" },
    { TokKind::StarEq, "*=" },
    { TokKind::SlashEq, "/=" },
    { TokKind::PercentEq, "%=" }
};

} // namespace

TokKind ClassifyKeyword(std::string_view ident) noexcept
{
    for (const auto& kw : kKeywords)
    {
        if (ident == kw.text)
        {
            return kw.kind;
        }
    }

    return TokKind::Ident;
}

std::string_view TokSpelling(TokKind kind) noexcept
{
    for (const auto& kw : kKeywords)
    {
        if (kw.kind == kind)
        {
            return kw.text;
        }
    }

    for (const auto& p : kPunct)
    {
        if (p.kind == kind)
        {
            return p.text;
        }
    }

    switch (kind)
    {
    case TokKind::Eof:
        return "<eof>";
    case TokKind::Ident:
        return "<ident>";
    default:
        return "<tok>";
    }
}

std::string_view TokName(TokKind kind) noexcept
{
    switch (kind)
    {
    case TokKind::Eof:
        return "end of input";
    case TokKind::Unknown:
        return "unknown token";
    case TokKind::Ident:
        return "identifier";
    case TokKind::IntLit:
        return "integer literal";
    case TokKind::FloatLit:
        return "float literal";
    case TokKind::BoolLit:
        return "bool literal";
    default:
        break;
    }

    // Keyword?
    for (const auto& kw : kKeywords)
    {
        if (kw.kind == kind)
        {
            return kw.text; // "return", "int", ...
        }
    }

    auto spelling = TokSpelling(kind);

    // Punctuation: wrap in quotes.
    return spelling;
}

} // namespace strata
