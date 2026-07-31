#pragma once

#include "AST/AST.h"
#include "Core/Diagnostics.h"
#include "Core/Util.h"
#include "Lex/Lexer.h"
#include "Lex/Token.h"
#include "Parse/Parser.h"
#include "Sema/ResolveOverloads.h"

#include <string.h>
#include <stdlib.h>

static inline Module* ParseModule(const char* src, DiagnosticEngine* diag, Arena* arena)
{
    Lexer lex;
    LexerInit(&lex, src, strlen(src), diag, 0);
    Parser parser;
    ParserInit(&parser, &lex, diag, arena, "test");
    return ParserParseModule(&parser);
}

static inline Module* ParseAndResolve(const char* src, DiagnosticEngine* diag, Arena* arena)
{
    Module* mod = ParseModule(src, diag, arena);
    if (mod)
    {
        ResolveOverloads(mod, diag, arena);
    }
    return mod;
}

typedef struct {
    Token* items;
    size_t count;
} TokenList;

static inline TokenList LexAll(const char* src)
{
    DiagnosticEngine diag;
    DiagnosticEngineInit(&diag);
    Lexer lex;
    LexerInit(&lex, src, strlen(src), &diag, 0);
    TokenList tl = {NULL, 0};
    size_t cap = 0;
    while (true)
    {
        Token t = LexerNextToken(&lex);
        if (tl.count >= cap)
        {
            cap = cap ? cap * 2 : 16;
            tl.items = (Token*)realloc(tl.items, cap * sizeof(Token));
        }
        tl.items[tl.count++] = t;
        if (t.kind == TokEof) break;
    }
    DiagnosticEngineFree(&diag);
    return tl;
}

static inline TokKind* Kinds(TokenList tl)
{
    TokKind* k = (TokKind*)malloc(tl.count * sizeof(TokKind));
    for (size_t i = 0; i < tl.count; i++)
    {
        k[i] = tl.items[i].kind;
    }
    return k;
}

static inline Str TextOf(const char* src, Token t)
{
    return (Str){src + t.range.start, t.range.length};
}
