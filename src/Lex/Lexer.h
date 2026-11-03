#pragma once

// Lexer interface.

#include "Core/Diagnostics.h"
#include "Core/SourceLocation.h"
#include "Lex/Token.h"

#include <stddef.h>

typedef struct
{
    const char* source;
    size_t sourceLen;
    DiagnosticEngine* diag;
    size_t pos;
    bool hasPeek;
    Token peeked;
    uint16_t fileId;
} Lexer;

void LexerInit(Lexer* lex, const char* source, size_t sourceLen, DiagnosticEngine* diag, uint16_t fileId);

Token LexerNextToken(Lexer* lex);
Token LexerPeekToken(Lexer* lex);

// Identifier character classes: a leading character is a letter or '_', later ones add digits.
bool IsIdentStart(char c);
bool IsIdentCont(char c);

size_t LexerPosition(const Lexer* lex);
bool LexerAtEnd(const Lexer* lex);
Str LexerSourceText(const Lexer* lex);

static inline char LexerPeek(const Lexer* lex, size_t ahead)
{
    size_t i = lex->pos + ahead;
    return (i < lex->sourceLen) ? lex->source[i] : '\0';
}
