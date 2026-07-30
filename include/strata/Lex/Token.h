// Strata compiler: lexical token kinds.
//
// Strata is an HLSL-inspired, C-style language: scalars and vector value types,
// `in`/`out`/`inout` parameter modifiers, and no pointers or references. The
// token set below covers the bootstrap surface area; new keywords and
// punctuation can be appended without disturbing existing enumerators.
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
    KwHalf,
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
std::string_view TokSpelling(TokKind k) noexcept;

// Human-readable name, e.g. "identifier", "'+'", "keyword 'return'".
std::string_view TokName(TokKind k) noexcept;

// Maps an identifier spelling to a keyword token kind, or TokKind::Ident if it
// is not a reserved word.
TokKind ClassifyKeyword(std::string_view ident) noexcept;

struct Token
{
    TokKind kind = TokKind::Eof;
    SourceRange range{};

    constexpr Token() = default;
    constexpr Token(TokKind k, SourceRange r) : kind(k), range(r)
    {
    }

    constexpr bool Is(TokKind k) const noexcept
    {
        return kind == k;
    }
    constexpr bool IsOneOf(TokKind a, TokKind b) const noexcept
    {
        return Is(a) || Is(b);
    }
    template <typename... Rest> constexpr bool IsOneOf(TokKind a, TokKind b, Rest... rest) const noexcept
    {
        return Is(a) || IsOneOf(b, rest...);
    }
};

} // namespace strata
