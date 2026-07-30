#pragma once

#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Token.h"

#include <cstddef>
#include <string_view>

namespace strata
{

class Lexer
{
  public:
    Lexer(std::string_view source, DiagnosticEngine& diag) noexcept : m_source(source), m_diag(diag)
    {
    }

    // Returns the next token and advances.
    Token NextToken();

    // Returns the next token without consuming it. Only one token of lookahead
    // is supported; calling peekToken() twice in a row returns the same token.
    Token PeekToken();

    std::size_t Position() const noexcept
    {
        return m_pos;
    }

    bool AtEnd() const noexcept
    {
        return m_pos >= m_source.size();
    }

    std::string_view SourceText() const noexcept
    {
        return m_source;
    }

  private:
    Token LexToken();
    Token LexIdentOrKeyword();
    Token LexNumber();
    Token LexLineComment();
    Token LexBlockComment();

    char Peek(std::size_t ahead = 0) const noexcept
    {
        std::size_t i = m_pos + ahead;
        return i < m_source.size() ? m_source[i] : '\0';
    }

    bool Bump() noexcept
    {
        if (m_pos < m_source.size())
        {
            ++m_pos;
            return true;
        }

        return false;
    }

    void SkipWhitespaceAndComments();

    Token Make(TokKind kind, std::size_t start) const noexcept
    {
        return Token {
            kind,
            {
                .start = static_cast<std::uint32_t>(start),
                .length = static_cast<std::uint16_t>(m_pos - start)
            }
        };
    }

    std::string_view m_source;

    DiagnosticEngine& m_diag;
    
    std::size_t m_pos = 0;
    
    bool m_hasPeek = false;
    Token m_peeked;
};

} // namespace strata
