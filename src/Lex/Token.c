#include "strata/Lex/Token.h"

#include <string.h>

typedef struct {
    TokKind kind;
    const char* text;
} Spelling;

static const Spelling kKeywords[] = {
    { TokKwVoid, "void" },
    { TokKwBool, "bool" },
    { TokKwInt, "int" },
    { TokKwUint, "uint" },
    { TokKwFloat, "float" },
    { TokKwDouble, "double" },
    { TokKwString, "string" },
    { TokKwIn, "in" },
    { TokKwOut, "out" },
    { TokKwInout, "inout" },
    { TokKwConst, "const" },
    { TokKwStatic, "static" },
    { TokKwExtern, "extern" },
    { TokKwHandle, "handle" },
    { TokKwReturn, "return" },
    { TokKwIf, "if" },
    { TokKwElse, "else" },
    { TokKwWhile, "while" },
    { TokKwFor, "for" },
    { TokKwBreak, "break" },
    { TokKwContinue, "continue" },
    { TokKwTrue, "true" },
    { TokKwFalse, "false" },
    { TokKwStruct, "struct" },
    { TokKwNamespace, "namespace" },
};

static const Spelling kPunct[] = {
    { TokLParen, "(" },
    { TokRParen, ")" },
    { TokLBrace, "{" },
    { TokRBrace, "}" },
    { TokLBracket, "[" },
    { TokRBracket, "]" },
    { TokComma, "," },
    { TokSemicolon, ";" },
    { TokColon, ":" },
    { TokDot, "." },
    { TokArrow, "->" },
    { TokAssign, "=" },
    { TokPlus, "+" },
    { TokMinus, "-" },
    { TokStar, "*" },
    { TokSlash, "/" },
    { TokPercent, "%" },
    { TokAmp, "&" },
    { TokPipe, "|" },
    { TokCaret, "^" },
    { TokTilde, "~" },
    { TokBang, "!" },
    { TokLt, "<" },
    { TokGt, ">" },
    { TokLtEq, "<=" },
    { TokGtEq, ">=" },
    { TokEqEq, "==" },
    { TokNotEq, "!=" },
    { TokAmpAmp, "&&" },
    { TokPipePipe, "||" },
    { TokShl, "<<" },
    { TokShr, ">>" },
    { TokPlusEq, "+=" },
    { TokMinusEq, "-=" },
    { TokStarEq, "*=" },
    { TokSlashEq, "/=" },
    { TokPercentEq, "%=" },
};

#define KEYWORD_COUNT (sizeof(kKeywords) / sizeof(kKeywords[0]))
#define PUNCT_COUNT (sizeof(kPunct) / sizeof(kPunct[0]))

TokKind ClassifyKeyword(Str ident)
{
    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (ident.len == strlen(kKeywords[i].text) &&
            memcmp(ident.data, kKeywords[i].text, ident.len) == 0)
        {
            return kKeywords[i].kind;
        }
    }
    return TokIdent;
}

const char* TokSpelling(TokKind kind)
{
    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (kKeywords[i].kind == kind)
        {
            return kKeywords[i].text;
        }
    }

    for (size_t i = 0; i < PUNCT_COUNT; i++)
    {
        if (kPunct[i].kind == kind)
        {
            return kPunct[i].text;
        }
    }

    switch (kind)
    {
    case TokEof:
        return "<eof>";
    case TokIdent:
        return "<ident>";
    default:
        return "<tok>";
    }
}

const char* TokName(TokKind kind)
{
    switch (kind)
    {
    case TokEof:
        return "end of input";
    case TokUnknown:
        return "unknown token";
    case TokIdent:
        return "identifier";
    case TokIntLit:
        return "integer literal";
    case TokFloatLit:
        return "float literal";
    case TokBoolLit:
        return "bool literal";
    default:
        break;
    }

    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (kKeywords[i].kind == kind)
        {
            return kKeywords[i].text;
        }
    }

    return TokSpelling(kind);
}
