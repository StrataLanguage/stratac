#include "Lex/Lexer.h"

#include <ctype.h>

static void SkipWhitespaceAndComments(Lexer* lex);
static Token LexTokenImpl(Lexer* lex);
static Token LexIdentOrKeyword(Lexer* lex);
static Token LexNumber(Lexer* lex);
static Token LexStringLiteral(Lexer* lex);

static bool IsIdentStart(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool IsIdentCont(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

static bool IsHexDigit(char c)
{
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static Token Make(const Lexer* lex, TokKind kind, size_t start)
{
    Token t = {0};
    t.kind = kind;
    t.range.start = (uint32_t)start;
    t.range.length = (uint16_t)(lex->m_pos - start);
    t.range.fileId = lex->m_fileId;

    return t;
}

static SourceRange LexRange(const Lexer* lex, size_t pos, uint16_t len)
{
    SourceRange r = {0};
    r.start = (uint32_t)pos;
    r.length = len;
    r.fileId = lex->m_fileId;

    return r;
}

void LexerInit(Lexer* lex, const char* source, size_t sourceLen, DiagnosticEngine* diag, uint16_t fileId)
{
    *lex = (Lexer){0};

    lex->m_source = source;
    lex->m_sourceLen = sourceLen;
    lex->m_diag = diag;
    lex->m_pos = 0;
    lex->m_hasPeek = false;
    lex->m_peeked.kind = TokEof;
    lex->m_peeked.range = SRC_INVALID;
    lex->m_fileId = fileId;
}

size_t LexerPosition(const Lexer* lex)
{
    return lex->m_pos;
}

bool LexerAtEnd(const Lexer* lex)
{
    return lex->m_pos >= lex->m_sourceLen;
}

Str LexerSourceText(const Lexer* lex)
{
    return (Str){lex->m_source, lex->m_sourceLen};
}

static void SkipWhitespaceAndComments(Lexer* lex)
{
    while (lex->m_pos < lex->m_sourceLen)
    {
        char c = lex->m_source[lex->m_pos];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
        {
            ++lex->m_pos;
        }
        else if (c == '/' && LexerPeek(lex, 1) == '/')
        {
            while (lex->m_pos < lex->m_sourceLen && lex->m_source[lex->m_pos] != '\n')
            {
                ++lex->m_pos;
            }
        }
        else if (c == '/' && LexerPeek(lex, 1) == '*')
        {
            int depth = 1;
            lex->m_pos += 2;

            while (lex->m_pos < lex->m_sourceLen && depth > 0)
            {
                if (lex->m_source[lex->m_pos] == '/' && LexerPeek(lex, 1) == '*')
                {
                    depth++;
                    lex->m_pos += 2;
                }
                else if (lex->m_source[lex->m_pos] == '*' && LexerPeek(lex, 1) == '/')
                {
                    depth--;
                    lex->m_pos += 2;
                }
                else
                {
                    ++lex->m_pos;
                }
            }

            if (depth > 0)
            {
                DiagError(lex->m_diag, LexRange(lex, lex->m_pos, 1), "unterminated block comment");
                return;
            }
        }
        else
        {
            break;
        }
    }
}

Token LexerNextToken(Lexer* lex)
{
    if (lex->m_hasPeek)
    {
        lex->m_hasPeek = false;

        Token token = lex->m_peeked;
        lex->m_peeked.kind = TokEof;
        lex->m_peeked.range = SRC_INVALID;

        return token;
    }

    return LexTokenImpl(lex);
}

Token LexerPeekToken(Lexer* lex)
{
    if (lex->m_hasPeek)
    {
        return lex->m_peeked;
    }

    lex->m_peeked = LexTokenImpl(lex);
    lex->m_hasPeek = true;

    return lex->m_peeked;
}

static Token LexTokenImpl(Lexer* lex)
{
    SkipWhitespaceAndComments(lex);

    size_t start = lex->m_pos;

    if (lex->m_pos >= lex->m_sourceLen)
    {
        return Make(lex, TokEof, start);
    }

    char c = lex->m_source[lex->m_pos];

    if (IsIdentStart(c))
    {
        return LexIdentOrKeyword(lex);
    }

    if (c == '"')
    {
        return LexStringLiteral(lex);
    }

    if (IsDigit(c))
    {
        return LexNumber(lex);
    }

    if (c == '.' && IsDigit(LexerPeek(lex, 1)))
    {
        return LexNumber(lex);
    }

    ++lex->m_pos;

    switch (c)
    {
    case '(':
        return Make(lex, TokLParen, start);
    case ')':
        return Make(lex, TokRParen, start);
    case '{':
        return Make(lex, TokLBrace, start);
    case '}':
        return Make(lex, TokRBrace, start);
    case '[':
        return Make(lex, TokLBracket, start);
    case ']':
        return Make(lex, TokRBracket, start);
    case ',':
        return Make(lex, TokComma, start);
    case ';':
        return Make(lex, TokSemicolon, start);
    case ':':
        return Make(lex, TokColon, start);
    case '~':
        return Make(lex, TokTilde, start);
    case '^':
        return Make(lex, TokCaret, start);
    case '.':
        return Make(lex, TokDot, start);
    case '-':
        if (LexerPeek(lex, 0) == '-')
        {
            ++lex->m_pos;
            
            return Make(lex, TokDec, start);
        }

        if (LexerPeek(lex, 0) == '>')
        {
            ++lex->m_pos;

            return Make(lex, TokArrow, start);
        }

        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokMinusEq, start);
        }

        return Make(lex, TokMinus, start);
    case '=':
        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokEqEq, start);
        }

        return Make(lex, TokAssign, start);
    case '!':
        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokNotEq, start);
        }

        return Make(lex, TokBang, start);
    case '+':
        if (LexerPeek(lex, 0) == '+')
        {
            ++lex->m_pos;

            return Make(lex, TokInc, start);
        }

        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokPlusEq, start);
        }

        return Make(lex, TokPlus, start);
    case '*':
        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokStarEq, start);
        }

        return Make(lex, TokStar, start);
    case '/':
        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokSlashEq, start);
        }

        return Make(lex, TokSlash, start);
    case '%':
        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokPercentEq, start);
        }

        return Make(lex, TokPercent, start);
    case '&':
        if (LexerPeek(lex, 0) == '&')
        {
            ++lex->m_pos;

            return Make(lex, TokAmpAmp, start);
        }

        return Make(lex, TokAmp, start);
    case '|':
        if (LexerPeek(lex, 0) == '|')
        {
            ++lex->m_pos;

            return Make(lex, TokPipePipe, start);
        }

        return Make(lex, TokPipe, start);
    case '<':
        if (LexerPeek(lex, 0) == '<')
        {
            ++lex->m_pos;

            return Make(lex, TokShl, start);
        }

        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokLtEq, start);
        }

        return Make(lex, TokLt, start);
    case '>':
        if (LexerPeek(lex, 0) == '>')
        {
            ++lex->m_pos;

            return Make(lex, TokShr, start);
        }

        if (LexerPeek(lex, 0) == '=')
        {
            ++lex->m_pos;

            return Make(lex, TokGtEq, start);
        }

        return Make(lex, TokGt, start);
    default:
        DiagErrorFmt(lex->m_diag, LexRange(lex, start, 1), "unexpected character '%c'", c);

        return Make(lex, TokUnknown, start);
    }
}

static Token LexIdentOrKeyword(Lexer* lex)
{
    size_t start = lex->m_pos;

    while (lex->m_pos < lex->m_sourceLen && IsIdentCont(lex->m_source[lex->m_pos]))
    {
        ++lex->m_pos;
    }

    Str text = {lex->m_source + start, lex->m_pos - start};
    TokKind keyword = ClassifyKeyword(text);

    if (keyword == TokKwTrue || keyword == TokKwFalse)
    {
        return Make(lex, TokBoolLit, start);
    }

    if (keyword != TokIdent)
    {
        return Make(lex, keyword, start);
    }

    return Make(lex, TokIdent, start);
}

static Token LexNumber(Lexer* lex)
{
    size_t start = lex->m_pos;
    bool isFloat = false;

    if (lex->m_source[lex->m_pos] == '0' && (LexerPeek(lex, 1) == 'x' || LexerPeek(lex, 1) == 'X'))
    {
        lex->m_pos += 2;

        while (lex->m_pos < lex->m_sourceLen && IsHexDigit(lex->m_source[lex->m_pos]))
        {
            ++lex->m_pos;
        }

        if (LexerPeek(lex, 0) == 'u' || LexerPeek(lex, 0) == 'U')
        {
            ++lex->m_pos;
        }

        return Make(lex, TokIntLit, start);
    }

    if (lex->m_source[lex->m_pos] == '.')
    {
        isFloat = true;
        
        ++lex->m_pos;

        while (lex->m_pos < lex->m_sourceLen && IsDigit(lex->m_source[lex->m_pos]))
        {
            ++lex->m_pos;
        }
    }
    else
    {
        while (lex->m_pos < lex->m_sourceLen && IsDigit(lex->m_source[lex->m_pos]))
        {
            ++lex->m_pos;
        }

        if (LexerPeek(lex, 0) == '.')
        {
            isFloat = true;

            ++lex->m_pos;

            while (lex->m_pos < lex->m_sourceLen && IsDigit(lex->m_source[lex->m_pos]))
            {
                ++lex->m_pos;
            }
        }
    }

    if (LexerPeek(lex, 0) == 'e' || LexerPeek(lex, 0) == 'E')
    {
        isFloat = true;

        ++lex->m_pos;

        if (LexerPeek(lex, 0) == '+' || LexerPeek(lex, 0) == '-')
        {
            ++lex->m_pos;
        }

        while (lex->m_pos < lex->m_sourceLen && IsDigit(lex->m_source[lex->m_pos]))
        {
            ++lex->m_pos;
        }
    }

    char suffix = LexerPeek(lex, 0);

    if (suffix == 'f' || suffix == 'F')
    {
        isFloat = true;

        ++lex->m_pos;
    }
    else if (suffix == 'u' || suffix == 'U')
    {
        ++lex->m_pos;
    }

    return Make(lex, isFloat ? TokFloatLit : TokIntLit, start);
}

static Token LexStringLiteral(Lexer* lex)
{
    size_t start = lex->m_pos;
    ++lex->m_pos;

    while (lex->m_pos < lex->m_sourceLen)
    {
        char c = lex->m_source[lex->m_pos];
        if (c == '"')
        {
            ++lex->m_pos;
            return Make(lex, TokStrLit, start);
        }

        if (c == '\\' && (LexerPeek(lex, 1) == '\\' || LexerPeek(lex, 1) == '"'
            || LexerPeek(lex, 1) == 'n' || LexerPeek(lex, 1) == 't'
            || LexerPeek(lex, 1) == 'r' || LexerPeek(lex, 1) == '0'))
        {
            lex->m_pos += 2;
            continue;
        }

        if (c == '\n')
        {
            DiagError(lex->m_diag, LexRange(lex, start, (uint16_t)(lex->m_pos - start + 1)), "unterminated string literal");
            return Make(lex, TokStrLit, start);
        }

        ++lex->m_pos;
    }

    DiagError(lex->m_diag, LexRange(lex, start, (uint16_t)(lex->m_pos - start)), "unterminated string literal");
    return Make(lex, TokStrLit, start);
}
