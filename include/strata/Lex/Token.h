#pragma once

#include "strata/Core/SourceLocation.h"

#include <cstdint>
#include <string_view>

namespace strata
{

enum class TokKind : std::uint8_t
{
    // --- Special ---
    Eof,     // end of input
    Unknown, // unrecognized character

    // --- Literals & identifiers ---
    Ident,
    IntLit,   // 123, 0x1F, 42u
    FloatLit, // 1.0, 1.5e10, 2.0f
    BoolLit,  // true, false

    // --- Keywords ---
    KwVoid,
    KwBool,
    KwInt,
    KwUint,
    KwFloat,
    KwDouble,
    KwString, // reserved
    KwIn,
    KwOut,
    KwInout,
    KwConst,
    KwStatic,
    KwExtern,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwBreak,
    KwContinue,
    KwTrue,
    KwFalse,
    KwStruct,
    KwHandle,
    KwNamespace, // reserved

    // --- Punctuation ---
    LParen,    // (
    RParen,    // )
    LBrace,    // {
    RBrace,    // }
    LBracket,  // [
    RBracket,  // ]
    Comma,     // ,
    Semicolon, // ;
    Colon,     // :
    Dot,       // .
    Arrow,     // ->

    // --- Operators ---
    Assign,    // =
    Plus,      // +
    Minus,     // -
    Star,      // *
    Slash,     // /
    Percent,   // %
    Amp,       // &
    Pipe,      // |
    Caret,     // ^
    Tilde,     // ~
    Bang,      // !
    Lt,        // <
    Gt,        // >
    LtEq,      // <=
    GtEq,      // >=
    EqEq,      // ==
    NotEq,     // !=
    AmpAmp,    // &&
    PipePipe,  // ||
    Shl,       // <<
    Shr,       // >>
    PlusEq,    // +=
    MinusEq,   // -=
    StarEq,    // *=
    SlashEq,   // /=
    PercentEq, // %=
};

// Canonical spelling for a token kind, where one exists. Used by the parser to
// emit messages like "expected ';' but found ','" and by diagnostics.
std::string_view TokSpelling(TokKind kind) noexcept;

// Human-readable name, e.g. "identifier", "'+'", "keyword 'return'".
std::string_view TokName(TokKind kind) noexcept;

// Maps an identifier spelling to a keyword token kind, or TokKind::Ident if it is not a reserved word.
TokKind ClassifyKeyword(std::string_view ident) noexcept;

struct Token
{
    TokKind kind = TokKind::Eof;
    SourceRange range{};

    constexpr Token() = default;
    constexpr Token(TokKind k, SourceRange r)
        : kind(k),
          range(r)
    {
    }

    constexpr bool Is(TokKind otherKind) const noexcept
    {
        return kind == otherKind;
    }

    constexpr bool IsOneOf(TokKind aKind, TokKind bKind) const noexcept
    {
        return Is(aKind) || Is(bKind);
    }

    template <typename... Rest>
    constexpr bool IsOneOf(TokKind aKind, TokKind bKind, Rest... rest) const noexcept
    {
        return Is(aKind) || IsOneOf(bKind, rest...);
    }
};

} // namespace strata
