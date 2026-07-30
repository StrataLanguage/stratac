#include "strata/Lex/Lexer.h"

#include <cctype>

namespace strata {

namespace {

bool isIdentStart(char c) noexcept { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentCont(char c) noexcept { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isHexDigit(char c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

void Lexer::skipWhitespaceAndComments() {
    while (pos_ < source_.size()) {
        char c = source_[pos_];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            // line comment
            while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
        } else if (c == '/' && peek(1) == '*') {
            // block comment (HLSL nests block comments)
            int depth = 1;
            pos_ += 2;
            while (pos_ < source_.size() && depth > 0) {
                if (source_[pos_] == '/' && peek(1) == '*') {
                    depth++; pos_ += 2;
                } else if (source_[pos_] == '*' && peek(1) == '/') {
                    depth--; pos_ += 2;
                } else {
                    ++pos_;
                }
            }
            if (depth > 0) {
                diag_.error({static_cast<std::uint32_t>(pos_), 1}, "unterminated block comment");
                return;
            }
        } else {
            break;
        }
    }
}

Token Lexer::nextToken() {
    if (hasPeek_) {
        hasPeek_ = false;
        Token t = peeked_;
        peeked_ = {};
        return t;
    }
    return lexToken();
}

Token Lexer::peekToken() {
    if (hasPeek_) return peeked_;
    peeked_ = lexToken();
    hasPeek_ = true;
    return peeked_;
}

Token Lexer::lexToken() {
    skipWhitespaceAndComments();
    std::size_t start = pos_;
    if (pos_ >= source_.size()) return {TokKind::Eof, {static_cast<std::uint32_t>(pos_), 0}};

    char c = source_[pos_];

    if (isIdentStart(c)) return lexIdentOrKeyword();
    if (isDigit(c)) return lexNumber();
    if (c == '.' && isDigit(peek(1))) return lexNumber(); // leading-dot float

    // Punctuation / operators
    ++pos_;
    switch (c) {
        case '(': return make(TokKind::LParen, start);
        case ')': return make(TokKind::RParen, start);
        case '{': return make(TokKind::LBrace, start);
        case '}': return make(TokKind::RBrace, start);
        case '[': return make(TokKind::LBracket, start);
        case ']': return make(TokKind::RBracket, start);
        case ',': return make(TokKind::Comma, start);
        case ';': return make(TokKind::Semicolon, start);
        case ':': return make(TokKind::Colon, start);
        case '~': return make(TokKind::Tilde, start);
        case '^': return make(TokKind::Caret, start);
        case '.': return make(TokKind::Dot, start);
        case '-':
            if (peek() == '>') { ++pos_; return make(TokKind::Arrow, start); }
            if (peek() == '=') { ++pos_; return make(TokKind::MinusEq, start); }
            return make(TokKind::Minus, start);
        case '=':
            if (peek() == '=') { ++pos_; return make(TokKind::EqEq, start); }
            return make(TokKind::Assign, start);
        case '!':
            if (peek() == '=') { ++pos_; return make(TokKind::NotEq, start); }
            return make(TokKind::Bang, start);
        case '+':
            if (peek() == '=') { ++pos_; return make(TokKind::PlusEq, start); }
            return make(TokKind::Plus, start);
        case '*':
            if (peek() == '=') { ++pos_; return make(TokKind::StarEq, start); }
            return make(TokKind::Star, start);
        case '/':
            if (peek() == '=') { ++pos_; return make(TokKind::SlashEq, start); }
            return make(TokKind::Slash, start);
        case '%':
            if (peek() == '=') { ++pos_; return make(TokKind::PercentEq, start); }
            return make(TokKind::Percent, start);
        case '&':
            if (peek() == '&') { ++pos_; return make(TokKind::AmpAmp, start); }
            return make(TokKind::Amp, start);
        case '|':
            if (peek() == '|') { ++pos_; return make(TokKind::PipePipe, start); }
            return make(TokKind::Pipe, start);
        case '<':
            if (peek() == '<') { ++pos_; return make(TokKind::Shl, start); }
            if (peek() == '=') { ++pos_; return make(TokKind::LtEq, start); }
            return make(TokKind::Lt, start);
        case '>':
            if (peek() == '>') { ++pos_; return make(TokKind::Shr, start); }
            if (peek() == '=') { ++pos_; return make(TokKind::GtEq, start); }
            return make(TokKind::Gt, start);
        default:
            diag_.error({static_cast<std::uint32_t>(start), 1},
                        std::string("unexpected character '") + c + "'");
            return make(TokKind::Unknown, start);
    }
}

Token Lexer::lexIdentOrKeyword() {
    std::size_t start = pos_;
    while (pos_ < source_.size() && isIdentCont(source_[pos_])) ++pos_;
    std::string_view text(source_.data() + start, pos_ - start);

    TokKind kw = classifyKeyword(text);
    if (kw == TokKind::Kw_true || kw == TokKind::Kw_false) {
        return make(TokKind::BoolLit, start);
    }
    if (kw != TokKind::Ident) {
        return make(kw, start);
    }
    return make(TokKind::Ident, start);
}

Token Lexer::lexNumber() {
    std::size_t start = pos_;
    bool isFloat = false;

    if (source_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        // hexadecimal integer
        pos_ += 2;
        while (pos_ < source_.size() && isHexDigit(source_[pos_])) ++pos_;
        if (peek() == 'u' || peek() == 'U') ++pos_;
        return make(TokKind::IntLit, start);
    }

    if (source_[pos_] == '.') {
        isFloat = true;
        ++pos_;
        while (pos_ < source_.size() && isDigit(source_[pos_])) ++pos_;
    } else {
        while (pos_ < source_.size() && isDigit(source_[pos_])) ++pos_;
        if (peek() == '.') {
            isFloat = true;
            ++pos_;
            while (pos_ < source_.size() && isDigit(source_[pos_])) ++pos_;
        }
    }

    // optional exponent
    if (peek() == 'e' || peek() == 'E') {
        isFloat = true;
        ++pos_;
        if (peek() == '+' || peek() == '-') ++pos_;
        while (pos_ < source_.size() && isDigit(source_[pos_])) ++pos_;
    }

    // optional numeric suffix
    char sfx = peek();
    if (sfx == 'f' || sfx == 'F' || sfx == 'h' || sfx == 'H') {
        isFloat = true;
        ++pos_;
    } else if (sfx == 'u' || sfx == 'U') {
        ++pos_;
    }

    return make(isFloat ? TokKind::FloatLit : TokKind::IntLit, start);
}

} // namespace strata
