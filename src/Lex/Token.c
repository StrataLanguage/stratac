#include "Lex/Token.h"

#include <string.h>

typedef struct {
    TokKind kind;
    const char* text;
} Spelling;

static const Spelling keywords[] = {
    { TokKwVoid, "void" },
    { TokKwBool, "bool" },
    { TokKwInt, "int" },
    { TokKwUint, "uint" },
    { TokKwLong, "long" },
    { TokKwUlong, "ulong" },
    { TokKwByte, "byte" },
    { TokKwSbyte, "sbyte" },
    { TokKwShort, "short" },
    { TokKwUshort, "ushort" },
    { TokKwFloat, "float" },
    { TokKwDouble, "double" },
    { TokKwString, "string" },
    { TokKwRef, "ref" },
    { TokKwConst, "const" },
    { TokKwStatic, "static" },
    { TokKwExtern, "extern" },
    { TokKwHandle, "handle" },
    { TokKwBox, "box" },
    { TokKwExtends, "extends" },
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
    { TokKwImport, "import" },
    { TokKwNamespace, "namespace" }
};

static const Spelling punct[] = {
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
    { TokInc, "++" },
    { TokDec, "--" }
};

#define KEYWORD_COUNT (sizeof(keywords) / sizeof(keywords[0]))
#define PUNCT_COUNT (sizeof(punct) / sizeof(punct[0]))

TokKind ClassifyKeyword(Str ident)
{
    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (ident.len == strlen(keywords[i].text) && memcmp(ident.data, keywords[i].text, ident.len) == 0)
        {
            return keywords[i].kind;
        }
    }

    return TokIdent;
}

const char* TokSpelling(TokKind kind)
{
    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (keywords[i].kind == kind)
        {
            return keywords[i].text;
        }
    }

    for (size_t i = 0; i < PUNCT_COUNT; i++)
    {
        if (punct[i].kind == kind)
        {
            return punct[i].text;
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
        return "end-of-input";
    case TokUnknown:
        return "unknown";
    case TokIdent:
        return "identifier";
    case TokIntLit:
        return "int-literal";
    case TokFloatLit:
        return "float-literal";
    case TokBoolLit:
        return "bool-literal";
    case TokStrLit:
        return "string-literal";
    default:
        break;
    }

    for (size_t i = 0; i < KEYWORD_COUNT; i++)
    {
        if (keywords[i].kind == kind)
        {
            return keywords[i].text;
        }
    }

    return TokSpelling(kind);
}
