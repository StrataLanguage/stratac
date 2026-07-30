// Strata compiler: lexer.
//
// Turns a source buffer into a stream of Tokens on demand. The lexer is meant
// to be driven by the parser via nextToken()/peekToken(). Comments (// and
// /* */, nested) and whitespace are skipped. Literals are recognized but their
// exact numeric values are parsed later by the parser/semantic layer via
// SourceManager::slice().
#pragma once

#include "strata/Core/Diagnostics.h"
#include "strata/Core/SourceLocation.h"
#include "strata/Lex/Token.h"

#include <cstddef>
#include <string_view>

namespace strata {

class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diag) noexcept
        : source_(source), diag_(diag) {}

    // Returns the next token and advances.
    Token nextToken();

    // Returns the next token without consuming it. Only one token of lookahead
    // is supported; calling peekToken() twice in a row returns the same token.
    Token peekToken();

    std::size_t position() const noexcept { return pos_; }
    bool atEnd() const noexcept { return pos_ >= source_.size(); }

    std::string_view sourceText() const noexcept { return source_; }

private:
    Token lexToken();
    Token lexIdentOrKeyword();
    Token lexNumber();
    Token lexLineComment();
    Token lexBlockComment();

    char peek(std::size_t ahead = 0) const noexcept {
        std::size_t i = pos_ + ahead;
        return i < source_.size() ? source_[i] : '\0';
    }
    bool bump() noexcept {
        if (pos_ < source_.size()) { ++pos_; return true; }
        return false;
    }
    void skipWhitespaceAndComments();

    Token make(TokKind k, std::size_t start) const noexcept {
        return Token{k, {static_cast<std::uint32_t>(start),
                         static_cast<std::uint16_t>(pos_ - start)}};
    }

    std::string_view source_;
    DiagnosticEngine& diag_;
    std::size_t pos_ = 0;
    bool hasPeek_ = false;
    Token peeked_{};
};

} // namespace strata
