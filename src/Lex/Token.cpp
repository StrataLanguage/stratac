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
constexpr std::array<Spelling, 28> kKeywords = {{
    {
        .kind = TokKind::KwVoid,
        .text = "void",
    },
    {
        .kind = TokKind::KwBool,
        .text = "bool",
    },
    {
        .kind = TokKind::KwInt,
        .text = "int",
    },
    {
        .kind = TokKind::KwUint,
        .text = "uint",
    },
    {
        .kind = TokKind::KwHalf,
        .text = "half",
    },
    {
        .kind = TokKind::KwFloat,
        .text = "float",
    },
    {
        .kind = TokKind::KwDouble,
        .text = "double",
    },
    {
        .kind = TokKind::KwString,
        .text = "string",
    },
    {
        .kind = TokKind::KwIn,
        .text = "in",
    },
    {
        .kind = TokKind::KwOut,
        .text = "out",
    },
    {
        .kind = TokKind::KwInout,
        .text = "inout",
    },
    {
        .kind = TokKind::KwConst,
        .text = "const",
    },
    {
        .kind = TokKind::KwStatic,
        .text = "static",
    },
    {
        .kind = TokKind::KwExtern,
        .text = "extern",
    },
    {
        .kind = TokKind::KwHandle,
        .text = "handle",
    },
    {
        .kind = TokKind::KwReturn,
        .text = "return",
    },
    {
        .kind = TokKind::KwIf,
        .text = "if",
    },
    {
        .kind = TokKind::KwElse,
        .text = "else",
    },
    {
        .kind = TokKind::KwWhile,
        .text = "while",
    },
    {
        .kind = TokKind::KwFor,
        .text = "for",
    },
    {
        .kind = TokKind::KwBreak,
        .text = "break",
    },
    {
        .kind = TokKind::KwContinue,
        .text = "continue",
    },
    {
        .kind = TokKind::KwTrue,
        .text = "true",
    },
    {
        .kind = TokKind::KwFalse,
        .text = "false",
    },
    {
        .kind = TokKind::KwStruct,
        .text = "struct",
    },
    {
        .kind = TokKind::KwNamespace,
        .text = "namespace",
    },
}};

constexpr std::array<Spelling, 38> kPunct = {{
    {
        .kind = TokKind::LParen,
        .text = "(",
    },
    {
        .kind = TokKind::RParen,
        .text = ")",
    },
    {
        .kind = TokKind::LBrace,
        .text = "{",
    },
    {
        .kind = TokKind::RBrace,
        .text = "}",
    },
    {
        .kind = TokKind::LBracket,
        .text = "[",
    },
    {
        .kind = TokKind::RBracket,
        .text = "]",
    },
    {
        .kind = TokKind::Comma,
        .text = ",",
    },
    {
        .kind = TokKind::Semicolon,
        .text = ";",
    },
    {
        .kind = TokKind::Colon,
        .text = ":",
    },
    {
        .kind = TokKind::Dot,
        .text = ".",
    },
    {
        .kind = TokKind::Arrow,
        .text = "->",
    },
    {
        .kind = TokKind::Assign,
        .text = "=",
    },
    {
        .kind = TokKind::Plus,
        .text = "+",
    },
    {
        .kind = TokKind::Minus,
        .text = "-",
    },
    {
        .kind = TokKind::Star,
        .text = "*",
    },
    {
        .kind = TokKind::Slash,
        .text = "/",
    },
    {
        .kind = TokKind::Percent,
        .text = "%",
    },
    {
        .kind = TokKind::Amp,
        .text = "&",
    },
    {
        .kind = TokKind::Pipe,
        .text = "|",
    },
    {
        .kind = TokKind::Caret,
        .text = "^",
    },
    {
        .kind = TokKind::Tilde,
        .text = "~",
    },
    {
        .kind = TokKind::Bang,
        .text = "!",
    },
    {
        .kind = TokKind::Lt,
        .text = "<",
    },
    {
        .kind = TokKind::Gt,
        .text = ">",
    },
    {
        .kind = TokKind::LtEq,
        .text = "<=",
    },
    {
        .kind = TokKind::GtEq,
        .text = ">=",
    },
    {
        .kind = TokKind::EqEq,
        .text = "==",
    },
    {
        .kind = TokKind::NotEq,
        .text = "!=",
    },
    {
        .kind = TokKind::AmpAmp,
        .text = "&&",
    },
    {
        .kind = TokKind::PipePipe,
        .text = "||",
    },
    {
        .kind = TokKind::Shl,
        .text = "<<",
    },
    {
        .kind = TokKind::Shr,
        .text = ">>",
    },
    {
        .kind = TokKind::PlusEq,
        .text = "+=",
    },
    {
        .kind = TokKind::MinusEq,
        .text = "-=",
    },
    {
        .kind = TokKind::StarEq,
        .text = "*=",
    },
    {
        .kind = TokKind::SlashEq,
        .text = "/=",
    },
    {
        .kind = TokKind::PercentEq,
        .text = "%=",
    },
}};

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
