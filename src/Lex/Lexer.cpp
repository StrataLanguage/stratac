#include "strata/Lex/Lexer.h"

#include <cctype>

namespace strata
{

namespace
{

bool IsIdentStart(char c) noexcept
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentCont(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool IsHexDigit(char c) noexcept
{
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

void Lexer::SkipWhitespaceAndComments()
{
    while (m_pos < m_source.size())
    {
        char c = m_source[m_pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
        {
            ++m_pos;
        }
        else if (c == '/' && Peek(1) == '/')
        {
            // line comment
            while (m_pos < m_source.size() && m_source[m_pos] != '\n')
            {
                ++m_pos;
            }
        }
        else if (c == '/' && Peek(1) == '*')
        {
            // block comment (HLSL nests block comments)
            int depth = 1;
            m_pos += 2;
            while (m_pos < m_source.size() && depth > 0)
            {
                if (m_source[m_pos] == '/' && Peek(1) == '*')
                {
                    depth++;
                    m_pos += 2;
                }
                else if (m_source[m_pos] == '*' && Peek(1) == '/')
                {
                    depth--;
                    m_pos += 2;
                }
                else
                {
                    ++m_pos;
                }
            }

            if (depth > 0)
            {
                m_diag.Error({static_cast<std::uint32_t>(m_pos), 1}, "unterminated block comment");
                return;
            }
        }
        else
        {
            break;
        }
    }
}

Token Lexer::NextToken()
{
    if (m_hasPeek)
    {
        m_hasPeek = false;
        Token t = m_peeked;
        m_peeked = {};
        return t;
    }

    return LexToken();
}

Token Lexer::PeekToken()
{
    if (m_hasPeek)
    {
        return m_peeked;
    }

    m_peeked = LexToken();
    m_hasPeek = true;
    return m_peeked;
}

Token Lexer::LexToken()
{
    SkipWhitespaceAndComments();
    std::size_t start = m_pos;
    if (m_pos >= m_source.size())
    {
        return {TokKind::Eof, {.start = static_cast<std::uint32_t>(m_pos), .length = 0}};
    }

    char c = m_source[m_pos];

    if (IsIdentStart(c))
    {
        return LexIdentOrKeyword();
    }

    if (IsDigit(c))
    {
        return LexNumber();
    }

    if (c == '.' && IsDigit(Peek(1)))
    {
        return LexNumber(); // leading-dot float
    }

    // Punctuation / operators
    ++m_pos;
    switch (c)
    {
    case '(':
        return Make(TokKind::LParen, start);
    case ')':
        return Make(TokKind::RParen, start);
    case '{':
        return Make(TokKind::LBrace, start);
    case '}':
        return Make(TokKind::RBrace, start);
    case '[':
        return Make(TokKind::LBracket, start);
    case ']':
        return Make(TokKind::RBracket, start);
    case ',':
        return Make(TokKind::Comma, start);
    case ';':
        return Make(TokKind::Semicolon, start);
    case ':':
        return Make(TokKind::Colon, start);
    case '~':
        return Make(TokKind::Tilde, start);
    case '^':
        return Make(TokKind::Caret, start);
    case '.':
        return Make(TokKind::Dot, start);
    case '-':
        if (Peek() == '>')
        {
            ++m_pos;
            return Make(TokKind::Arrow, start);
        }

        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::MinusEq, start);
        }

        return Make(TokKind::Minus, start);
    case '=':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::EqEq, start);
        }

        return Make(TokKind::Assign, start);
    case '!':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::NotEq, start);
        }

        return Make(TokKind::Bang, start);
    case '+':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::PlusEq, start);
        }

        return Make(TokKind::Plus, start);
    case '*':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::StarEq, start);
        }

        return Make(TokKind::Star, start);
    case '/':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::SlashEq, start);
        }

        return Make(TokKind::Slash, start);
    case '%':
        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::PercentEq, start);
        }

        return Make(TokKind::Percent, start);
    case '&':
        if (Peek() == '&')
        {
            ++m_pos;
            return Make(TokKind::AmpAmp, start);
        }

        return Make(TokKind::Amp, start);
    case '|':
        if (Peek() == '|')
        {
            ++m_pos;
            return Make(TokKind::PipePipe, start);
        }

        return Make(TokKind::Pipe, start);
    case '<':
        if (Peek() == '<')
        {
            ++m_pos;
            return Make(TokKind::Shl, start);
        }

        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::LtEq, start);
        }

        return Make(TokKind::Lt, start);
    case '>':
        if (Peek() == '>')
        {
            ++m_pos;
            return Make(TokKind::Shr, start);
        }

        if (Peek() == '=')
        {
            ++m_pos;
            return Make(TokKind::GtEq, start);
        }

        return Make(TokKind::Gt, start);
    default:
        m_diag.Error({static_cast<std::uint32_t>(start), 1}, std::string("unexpected character '") + c + "'");
        return Make(TokKind::Unknown, start);
    }
}

Token Lexer::LexIdentOrKeyword()
{
    std::size_t start = m_pos;
    while (m_pos < m_source.size() && IsIdentCont(m_source[m_pos]))
    {
        ++m_pos;
    }

    std::string_view text(m_source.data() + start, m_pos - start);

    TokKind kw = ClassifyKeyword(text);
    if (kw == TokKind::KwTrue || kw == TokKind::KwFalse)
    {
        return Make(TokKind::BoolLit, start);
    }

    if (kw != TokKind::Ident)
    {
        return Make(kw, start);
    }

    return Make(TokKind::Ident, start);
}

Token Lexer::LexNumber()
{
    std::size_t start = m_pos;
    bool isFloat = false;

    if (m_source[m_pos] == '0' && (Peek(1) == 'x' || Peek(1) == 'X'))
    {
        // hexadecimal integer
        m_pos += 2;
        while (m_pos < m_source.size() && IsHexDigit(m_source[m_pos]))
        {
            ++m_pos;
        }

        if (Peek() == 'u' || Peek() == 'U')
        {
            ++m_pos;
        }

        return Make(TokKind::IntLit, start);
    }

    if (m_source[m_pos] == '.')
    {
        isFloat = true;
        ++m_pos;
        while (m_pos < m_source.size() && IsDigit(m_source[m_pos]))
        {
            ++m_pos;
        }
    }
    else
    {
        while (m_pos < m_source.size() && IsDigit(m_source[m_pos]))
        {
            ++m_pos;
        }

        if (Peek() == '.')
        {
            isFloat = true;
            ++m_pos;
            while (m_pos < m_source.size() && IsDigit(m_source[m_pos]))
            {
                ++m_pos;
            }
        }
    }

    // optional exponent
    if (Peek() == 'e' || Peek() == 'E')
    {
        isFloat = true;
        ++m_pos;
        if (Peek() == '+' || Peek() == '-')
        {
            ++m_pos;
        }
        while (m_pos < m_source.size() && IsDigit(m_source[m_pos]))
        {
            ++m_pos;
        }
    }

    // optional numeric suffix
    char sfx = Peek();
    if (sfx == 'f' || sfx == 'F' || sfx == 'h' || sfx == 'H')
    {
        isFloat = true;
        ++m_pos;
    }
    else if (sfx == 'u' || sfx == 'U')
    {
        ++m_pos;
    }

    return Make(isFloat ? TokKind::FloatLit : TokKind::IntLit, start);
}

} // namespace strata
