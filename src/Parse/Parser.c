// Parser entry: module parsing, attributes and trial-parse rollback.

#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

SourceRange SpanFrom(Token begin, Token end)
{
    uint32_t s = begin.range.start;
    uint32_t e = SourceRangeEnd(end.range);

    return (SourceRange){s, (uint32_t)(e > s ? e - s : 0), begin.range.fileId};
}

SourceRange SpanToCur(Parser* p, SourceRange from)
{
    return (SourceRange){from.start, (uint32_t)(p->cur.range.start - from.start), from.fileId};
}

LexerCheckpoint SaveLexerState(Parser* p)
{
    LexerCheckpoint c;
    c.pos = LexerPosition(p->lex);
    c.hasPeek = p->lex->hasPeek;
    c.peeked = p->lex->peeked;
    c.cur = p->cur;
    c.diagCount = DiagCount(p->diag);
    return c;
}

// Rewinds to the checkpoint and drops every diagnostic the abandoned trial
// parse reported - parser errors and lexer errors alike. Re-lexing the same
// tokens reports lexer errors again, so each one ends up reported exactly once.
void RestoreLexerState(Parser* p, LexerCheckpoint c)
{
    p->lex->pos = c.pos;
    p->lex->hasPeek = c.hasPeek;
    p->lex->peeked = c.peeked;
    p->cur = c.cur;
    DiagTruncate(p->diag, c.diagCount);
}

void ParserInit(Parser* p, Lexer* lex, DiagnosticEngine* diag, Arena* arena, const char* moduleName)
{
    p->lex = lex;
    p->diag = diag;
    p->arena = arena;
    p->moduleName = ArenaStrdup(arena, moduleName ? moduleName : "strata_module");
    p->cur = LexerNextToken(lex);
    p->returnType = NULL;
    p->hasReturnStmt = false;
    p->implSelfType = NULL;
}

Str ParserIdentText(const Parser* p, Token t)
{
    Str src = LexerSourceText(p->lex);

    if (t.range.start >= src.len)
    {
        return STR_EMPTY;
    }

    uint32_t end = SourceRangeEnd(t.range);
    if (end > (uint32_t)src.len)
    {
        end = (uint32_t)src.len;
    }

    return (Str){src.data + t.range.start, end - t.range.start};
}

void Advance(Parser* p)
{
    p->cur = LexerNextToken(p->lex);
}

bool ParserConsume(Parser* p, TokKind k)
{
    if (p->cur.kind == k)
    {
        Advance(p);

        return true;
    }

    return false;
}

Token ParserExpect(Parser* p, TokKind k, const char* what)
{
    if (p->cur.kind == k)
    {
        Token token = p->cur;
        Advance(p);

        return token;
    }

    DiagErrorFmt(p->diag, p->cur.range, "expected %s but found '%s'", what, TokSpelling(p->cur.kind));

    return (Token){TokEof, SRC_INVALID};
}

void Synchronize(Parser* p)
{
    while (p->cur.kind != TokEof)
    {
        if (p->cur.kind == TokSemicolon)
        {
            Advance(p);
            return;
        }

        if (p->cur.kind == TokRBrace)
        {
            return;
        }

        Advance(p);
    }
}

static ImportDecl* ParseImport(Parser* p)
{
    Token kw = p->cur;
    Advance(p);

    if (p->cur.kind != TokIdent && p->cur.kind != TokDot)
    {
        DiagErrorFmt(p->diag, p->cur.range, "expected module path after 'import' but found '%s'",
                     TokSpelling(p->cur.kind));
        Synchronize(p);
        return NULL;
    }

    Token first = p->cur;
    Token last = first;

    while (p->cur.kind == TokIdent || p->cur.kind == TokSlash || p->cur.kind == TokDot)
    {
        last = p->cur;
        Advance(p);
    }

    SourceRange pathRange = SpanFrom(first, last);

    ParserExpect(p, TokSemicolon, "';'");

    ImportDecl* imp = AST_NEW(p->arena, ImportDecl);
    imp->base.kind = NodeImport;
    imp->base.range = SpanFrom(kw, last);
    imp->pathRange = pathRange;

    Str src = LexerSourceText(p->lex);
    uint32_t pend = SourceRangeEnd(pathRange);
    if (pend > (uint32_t)src.len)
    {
        pend = (uint32_t)src.len;
    }
    Str pathStr = {src.data + pathRange.start, pend - pathRange.start};
    imp->importPath = ToOwned(p->arena, pathStr);

    return imp;
}

static void AddTopLevelDecl(Module* mod, Node* decl, Parser* p)
{
    if (!decl)
    {
        Synchronize(p);
        return;
    }

    if (decl->kind == NodeFunction && ((FunctionDecl*)decl)->isDelegate)
    {
        VecPush(&mod->delegates, decl);
    }
    else if (decl->kind == NodeFunction && ((FunctionDecl*)decl)->isMeta)
    {
        VecPush(&mod->metaFunctions, decl);
    }
    else if (decl->kind == NodeFunction)
    {
        VecPush(&mod->functions, decl);
    }
    else if (decl->kind == NodeGlobal)
    {
        VecPush(&mod->globals, decl);
    }
}

// `@attr... enum`: the only attribute an enum takes is @flags.
static void ParseAttributedEnum(Parser* p, Module* mod, const Vec* attributes)
{
    EnumDecl* ed = ParseEnumDecl(p);

    for (size_t i = 0; i < attributes->count; i++)
    {
        const Attribute* attribute = (const Attribute*)VecGet(attributes, i);

        if (strcmp(attribute->name, "flags") == 0)
        {
            if (ed)
            {
                ed->isFlags = true;
            }
        }
        else
        {
            DiagError(p->diag, attribute->range, "the only attribute an enum takes is @flags");
        }
    }

    if (ed)
    {
        VecPush(&mod->enums, ed);
    }
    else
    {
        Synchronize(p);
    }
}

// `@attr... struct`, or `@attr... extern struct`: struct attributes are applied to the body.
static void ParseAttributedStruct(Parser* p, Module* mod, Vec* attributes, bool isExternStruct)
{
    if (isExternStruct)
    {
        Advance(p);
    }

    StructDecl* sd = ParseStructDecl(p, isExternStruct);

    if (sd)
    {
        ApplyStructAttributes(p, sd, attributes);
        VecPush(&mod->structs, sd);
    }
    else
    {
        Synchronize(p);
    }
}

// A declaration that starts with `@`: attributes, then an enum, a struct or a function.
static void ParseAttributedDecl(Parser* p, Module* mod)
{
    Vec attributes;
    VecInit(&attributes);
    ParseAttributes(p, &attributes);

    TokKind afterExtern = LexerPeekToken(p->lex).kind;
    bool isExternStruct = p->cur.kind == TokKwExtern && (afterExtern == TokKwStruct || afterExtern == TokKwUnion);

    if (p->cur.kind == TokKwEnum)
    {
        ParseAttributedEnum(p, mod, &attributes);
    }
    else if (p->cur.kind == TokKwStruct || p->cur.kind == TokKwUnion || isExternStruct)
    {
        ParseAttributedStruct(p, mod, &attributes, isExternStruct);
    }
    else
    {
        AddTopLevelDecl(mod, ParseFunctionWithAttributes(p, &attributes), p);
    }

    free(attributes.items);
}

static void ParseTopLevelImport(Parser* p, Module* mod)
{
    ImportDecl* imp = ParseImport(p);
    if (imp)
    {
        VecPush(&mod->imports, imp);
    }
    else
    {
        Synchronize(p);
    }
}

static void ParseTopLevelHandle(Parser* p, Module* mod)
{
    HandleDecl* hd = ParseHandleDecl(p);
    if (hd)
    {
        VecPush(&mod->handles, hd);
    }
    else
    {
        Synchronize(p);
    }
}

static void ParseTopLevelStruct(Parser* p, Module* mod)
{
    StructDecl* sd = ParseStructDecl(p, false);
    if (sd)
    {
        VecPush(&mod->structs, sd);
    }
    else
    {
        Synchronize(p);
    }
}

// `extern struct` / `extern union` with no attributes.
static void ParseTopLevelExternStruct(Parser* p, Module* mod)
{
    Advance(p);

    StructDecl* sd = ParseStructDecl(p, true);
    if (sd)
    {
        VecPush(&mod->structs, sd);
    }
    else
    {
        Synchronize(p);
    }
}

static void ParseTopLevelEnum(Parser* p, Module* mod)
{
    EnumDecl* ed = ParseEnumDecl(p);
    if (ed)
    {
        VecPush(&mod->enums, ed);
    }
    else
    {
        Synchronize(p);
    }
}

static void ParseTopLevelImpl(Parser* p, Module* mod)
{
    ImplDecl* impl = ParseImplDecl(p);

    if (impl)
    {
        VecPush(&mod->impls, impl);

        // Hoist extern methods into the module function list under
        // their qualified symbol (`Camera_GetFOV`) so module merging,
        // sema and codegen treat them like any other extern.
        for (size_t i = 0; i < impl->methods.count; i++)
        {
            VecPush(&mod->functions, VecGet(&impl->methods, i));
        }
    }
    else
    {
        Synchronize(p);
    }
}

Module* ParserParseModule(Parser* p)
{
    Module* mod = AST_NEW(p->arena, Module);
    mod->base.kind = NodeModule;
    mod->base.range = SRC_INVALID;
    mod->name = p->moduleName;
    VecInit(&mod->structs);
    VecInit(&mod->handles);
    VecInit(&mod->enums);
    VecInit(&mod->functions);
    VecInit(&mod->globals);
    VecInit(&mod->imports);
    VecInit(&mod->impls);
    VecInit(&mod->delegates);
    VecInit(&mod->genericTemplates);
    VecInit(&mod->metaFunctions);

    while (p->cur.kind != TokEof)
    {
        if (p->cur.kind == TokSemicolon || p->cur.kind == TokRBrace)
        {
            Advance(p);
            continue;
        }

        if (p->cur.kind == TokAt)
        {
            ParseAttributedDecl(p, mod);
            continue;
        }

        if (p->cur.kind == TokKwImport)
        {
            ParseTopLevelImport(p, mod);
            continue;
        }

        if (AtHandleDecl(p))
        {
            ParseTopLevelHandle(p, mod);
            continue;
        }

        if (p->cur.kind == TokKwStruct || p->cur.kind == TokKwUnion)
        {
            ParseTopLevelStruct(p, mod);
            continue;
        }

        if (p->cur.kind == TokKwEnum)
        {
            ParseTopLevelEnum(p, mod);
            continue;
        }

        if (p->cur.kind == TokKwImpl)
        {
            ParseTopLevelImpl(p, mod);
            continue;
        }

        if (p->cur.kind == TokKwExtern
            && (LexerPeekToken(p->lex).kind == TokKwStruct || LexerPeekToken(p->lex).kind == TokKwUnion))
        {
            ParseTopLevelExternStruct(p, mod);
            continue;
        }

        AddTopLevelDecl(mod, ParseFunction(p), p);
    }

    return mod;
}
