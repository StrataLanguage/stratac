#pragma once

// Parser helpers shared by the Parse/ files.

#include "Parse/Parser.h"

// Snapshot of lexer + parser position so a speculative parse can be undone without emitting diagnostics.
typedef struct
{
    size_t pos;
    bool hasPeek;
    Token peeked;
    Token cur;
    size_t diagCount;
} LexerCheckpoint;

SourceRange SpanFrom(Token begin, Token end);
char* ToOwned(Arena* arena, Str s);
SourceRange SpanToCur(Parser* p, SourceRange from);
uint64_t IntLiteralValueEx(Parser* p, Token tok, bool* outIsUnsigned, bool* outIsLong);
uint64_t IntLiteralValue(Parser* p, Token tok, bool* outIsUnsigned);
LexerCheckpoint SaveLexerState(Parser* p);
void RestoreLexerState(Parser* p, LexerCheckpoint c);
void Advance(Parser* p);
void Synchronize(Parser* p);
bool LooksLikeVarDecl(Parser* p);
bool AtHandleDecl(Parser* p);
void CheckTypeNameNotHandle(Parser* p, Token nameTok);
HandleDecl* ParseHandleDecl(Parser* p);
StructDecl* ParseStructDecl(Parser* p, bool isExtern);
EnumDecl* ParseEnumDecl(Parser* p);
void ParseAttributes(Parser* p, Vec* outAttributes);
void ApplyStructAttributes(Parser* p, StructDecl* sd, Vec* attributes);
Node* ParseFunction(Parser* p);
Node* ParseFunctionWithAttributes(Parser* p, const Vec* attributes);
ImplDecl* ParseImplDecl(Parser* p);
Node* ParseBlock(Parser* p);
Node* ParseExpr(Parser* p);
Node* ParseBinary(Parser* p, int minPrec);
Node* ParseStructInitBody(Parser* p, Token startTok, const char* typeName);
Node* ParseArrayInitBody(Parser* p, Token startTok, const TypeName* elementType);
