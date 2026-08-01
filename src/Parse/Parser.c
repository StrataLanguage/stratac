#include "Parse/Parser.h"

#include <stdlib.h>
#include <string.h>

static SourceRange SpanFrom(Token begin, Token end)
{
    uint32_t s = begin.range.start;
    uint32_t e = SourceRangeEnd(end.range);

    return (SourceRange){s, (uint16_t)(e > s ? e - s : 0), begin.range.fileId};
}

static bool BinaryInfo(TokKind k, int* prec, BinaryOp* op)
{
    switch (k)
    {
    case TokPipePipe: *prec = 1;  *op = BinLogicOr;     return true;
    case TokAmpAmp:   *prec = 2;  *op = BinLogicAnd;    return true;
    case TokPipe:     *prec = 3;  *op = BinBitOr;       return true;
    case TokCaret:    *prec = 4;  *op = BinBitXor;      return true;
    case TokAmp:      *prec = 5;  *op = BinBitAnd;      return true;
    case TokEqEq:     *prec = 6;  *op = BinEqEq;        return true;
    case TokNotEq:    *prec = 6;  *op = BinNotEq;       return true;
    case TokLt:       *prec = 7;  *op = BinLt;          return true;
    case TokLtEq:     *prec = 7;  *op = BinLtEq;        return true;
    case TokGt:       *prec = 7;  *op = BinGt;          return true;
    case TokGtEq:     *prec = 7;  *op = BinGtEq;        return true;
    case TokShl:      *prec = 8;  *op = BinShl;         return true;
    case TokShr:      *prec = 8;  *op = BinShr;         return true;
    case TokPlus:     *prec = 9;  *op = BinAdd;         return true;
    case TokMinus:    *prec = 9;  *op = BinSub;         return true;
    case TokStar:     *prec = 10; *op = BinMul;         return true;
    case TokSlash:    *prec = 10; *op = BinDiv;         return true;
    case TokPercent:  *prec = 10; *op = BinMod;         return true;
    default:                                            return false;
    }
}

static AssignOp MapAssign(TokKind k)
{
    switch (k)
    {
    case TokAssign:     return AssignSet;
    case TokPlusEq:     return AssignAdd;
    case TokMinusEq:    return AssignSub;
    case TokStarEq:     return AssignMul;
    case TokSlashEq:    return AssignDiv;
    case TokPercentEq:  return AssignMod;
    default:            return AssignSet;
    }
}

static bool IsAssignOp(TokKind k)
{
    return k == TokAssign
        || k == TokPlusEq
        || k == TokMinusEq
        || k == TokStarEq
        || k == TokSlashEq
        || k == TokPercentEq;
}

static char* ToOwned(Arena* arena, Str s)
{
    return arena_strndup(arena, s.data, s.len);
}

void ParserInit(Parser* p, Lexer* lex, DiagnosticEngine* diag, Arena* arena, const char* moduleName)
{
    p->m_lex = lex;
    p->m_diag = diag;
    p->m_arena = arena;
    p->m_moduleName = arena_strdup(arena, moduleName ? moduleName : "strata_module");
    p->m_cur = LexerNextToken(lex);
    p->m_returnType = NULL;
}

Str ParserIdentText(const Parser* p, Token t)
{
    Str src = LexerSourceText(p->m_lex);

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

static void Advance(Parser* p)
{
    p->m_cur = LexerNextToken(p->m_lex);
}

bool ParserConsume(Parser* p, TokKind k)
{
    if (p->m_cur.kind == k)
    {
        Advance(p);

        return true;
    }

    return false;
}

Token ParserExpect(Parser* p, TokKind k, const char* what)
{
    if (p->m_cur.kind == k)
    {
        Token token = p->m_cur;
        Advance(p);

        return token;
    }

    DiagErrorFmt(p->m_diag, p->m_cur.range, "expected %s but found '%s'", what, TokSpelling(p->m_cur.kind));

    return (Token){TokEof, SRC_INVALID};
}

static void Synchronize(Parser* p)
{
    while (p->m_cur.kind != TokEof)
    {
        if (p->m_cur.kind == TokSemicolon)
        {
            Advance(p);
            return;
        }

        if (p->m_cur.kind == TokRBrace)
        {
            return;
        }

        Advance(p);
    }
}

static bool LooksLikeVarDecl(const Parser* p)
{
    switch (p->m_cur.kind)
    {
    case TokKwConst:
    case TokKwVoid:
    case TokKwBool:
    case TokKwUint:
    case TokKwInt:
    case TokKwLong:
    case TokKwUlong:
    case TokKwFloat:
    case TokKwDouble:
    case TokKwString:
        return true;
    case TokIdent:
    {
        Token next = LexerPeekToken(p->m_lex);
        return next.kind == TokIdent;
    }
    default:
        return false;
    }
}

bool ParserTryParseType(Parser* p, TypeName* out)
{
    bool isConst = false;
    SourceRange constRange = p->m_cur.range;

    if (p->m_cur.kind == TokKwConst)
    {
        isConst = true;
        Advance(p);
    }

    const char* name = NULL;

    switch (p->m_cur.kind)
    {
    case TokKwVoid:   name = "void";   break;
    case TokKwBool:   name = "bool";   break;
    case TokKwInt:    name = "int";    break;
    case TokKwUint:   name = "uint";   break;
    case TokKwLong:   name = "long";   break;
    case TokKwUlong:  name = "ulong";  break;
    case TokKwFloat:  name = "float";  break;
    case TokKwDouble: name = "double"; break;
    case TokKwString: name = "string"; break;
    case TokIdent:    name = ToOwned(p->m_arena, ParserIdentText(p, p->m_cur)); break;
    default:
        if (isConst)
        {
            DiagError(p->m_diag, constRange, "'const' must be followed by a type");
        }

        return false;
    }

    Advance(p);

    out->name = (char*)name;
    out->range = (SourceRange){constRange.start, (uint16_t)(p->m_cur.range.start - constRange.start), constRange.fileId};
    out->isConst = isConst;

    return true;
}

static HandleDecl* ParseHandleDecl(Parser* p)
{
    if (!ParserConsume(p, TokKwHandle))
    {
        DiagError(p->m_diag, p->m_cur.range, "expected 'handle' keyword");
        return NULL;
    }

    if (p->m_cur.kind != TokIdent)
    {
        DiagError(p->m_diag, p->m_cur.range, "expected a handle name");
        return NULL;
    }

    Token nameTok = p->m_cur;
    Advance(p);

    HandleDecl* node = AST_NEW(p->m_arena, HandleDecl);
    node->base.kind = NodeHandle;
    node->base.range = nameTok.range;
    node->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));
    node->extendsName = NULL;

    if (ParserConsume(p, TokKwExtends))
    {
        if (p->m_cur.kind != TokIdent)
        {
            DiagError(p->m_diag, p->m_cur.range, "expected a base handle name after 'extends'");
        }
        else
        {
            Token baseTok = p->m_cur;
            Advance(p);
            node->extendsName = ToOwned(p->m_arena, ParserIdentText(p, baseTok));
        }
    }

    if (ParserConsume(p, TokLBrace))
    {
        DiagError(p->m_diag, nameTok.range, "opaque handles cannot have a body");
        Synchronize(p);
    }

    ParserExpect(p, TokSemicolon, "';'");

    return node;
}

static StructDecl* ParseStructDecl(Parser* p)
{
    if (!ParserConsume(p, TokKwStruct))
    {
        DiagError(p->m_diag, p->m_cur.range, "expected 'struct'");
        return NULL;
    }

    if (p->m_cur.kind != TokIdent)
    {
        DiagError(p->m_diag, p->m_cur.range, "expected a struct name");
        return NULL;
    }

    Token nameTok = p->m_cur;
    Advance(p);

    StructDecl* node = AST_NEW(p->m_arena, StructDecl);
    node->base.kind = NodeStruct;
    node->base.range = nameTok.range;
    node->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));
    VecInit(&node->fields);

    if (ParserConsume(p, TokLBrace))
    {
        while (p->m_cur.kind != TokRBrace && p->m_cur.kind != TokEof)
        {
            TypeName ft = {0};
            if (!ParserTryParseType(p, &ft))
            {
                DiagError(p->m_diag, p->m_cur.range, "expected a field type");
                break;
            }

            if (p->m_cur.kind != TokIdent)
            {
                DiagError(p->m_diag, p->m_cur.range, "expected a field name");
                break;
            }

            Token fieldTok = p->m_cur;
            Advance(p);

            FieldDecl* field = AST_NEW(p->m_arena, FieldDecl);
            field->type = ft;
            field->name = ToOwned(p->m_arena, ParserIdentText(p, fieldTok));
            VecPush(&node->fields, field);

            Token semi = ParserExpect(p, TokSemicolon, "';'");

            if (semi.kind != TokSemicolon)
            {
                break;
            }
        }

        ParserExpect(p, TokRBrace, "'}'");
    }
    else
    {
        DiagErrorFmt(p->m_diag, nameTok.range, "struct '%s' requires a body; use 'handle %s;' for an opaque type", node->name, node->name);
    }

    ParserExpect(p, TokSemicolon, "';'");

    return node;
}

static ParamDecl* ParseParam(Parser* p)
{
    SourceRange start = p->m_cur.range;
    ParamMod mod = ModNone;

    if (ParserConsume(p, TokKwRef))
    {
        mod = ModRef;
    }

    TypeName type = {0};

    if (!ParserTryParseType(p, &type))
    {
        DiagError(p->m_diag, p->m_cur.range, "expected a parameter type");
        return NULL;
    }

    if (p->m_cur.kind != TokIdent)
    {
        DiagError(p->m_diag, p->m_cur.range, "expected a parameter name");
        return NULL;
    }

    Token nameTok = p->m_cur;
    Advance(p);

    ParamDecl* node = AST_NEW(p->m_arena, ParamDecl);
    node->base.kind = NodeParam;
    node->base.range = (SourceRange){start.start, (uint16_t)(SourceRangeEnd(nameTok.range) - start.start), start.fileId};
    node->mod = mod;
    node->type = type;
    node->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));

    return node;
}

static Node* ParseBlock(Parser* p);
static Node* ParseStatement(Parser* p);
static Node* ParseExpr(Parser* p);
static Node* ParseAssign(Parser* p);
static Node* ParseBinary(Parser* p, int minPrec);
static Node* ParseUnary(Parser* p);
static Node* ParsePostfix(Parser* p);
static Node* ParsePrimary(Parser* p);
static Node* ParseStructInitBody(Parser* p, Token startTok, const char* typeName);

static Node* ParseFunction(Parser* p)
{
    bool isExtern = ParserConsume(p, TokKwExtern);

    TypeName returnType = {0};
    if (!ParserTryParseType(p, &returnType))
    {
        if (p->m_cur.kind != TokEof)
        {
            DiagError(p->m_diag, p->m_cur.range, "expected a type to declare a function");
        }

        return NULL;
    }

    if (p->m_cur.kind != TokIdent)
    {
        DiagError(p->m_diag, p->m_cur.range, "expected a name");

        return NULL;
    }

    Token nameTok = p->m_cur;
    Advance(p);

    if (p->m_cur.kind != TokLParen)
    {
        if (isExtern)
        {
            DiagError(p->m_diag, nameTok.range, "extern cannot be used with a global variable");
            Synchronize(p);

            return NULL;
        }

        GlobalDecl* gd = AST_NEW(p->m_arena, GlobalDecl);
        gd->base.kind = NodeGlobal;
        gd->base.range = nameTok.range;
        gd->type = returnType;
        gd->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));
        gd->init = NULL;

        if (ParserConsume(p, TokAssign))
        {
            gd->init = ParseExpr(p);
        }

        ParserExpect(p, TokSemicolon, "';'");

        return (Node*)gd;
    }

        FunctionDecl* node = AST_NEW(p->m_arena, FunctionDecl);
    node->base.kind = NodeFunction;
    node->base.range = SpanFrom((Token){TokIdent, returnType.range}, nameTok);
    node->returnType = returnType;
    node->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));
    node->mangledName = arena_strdup(p->m_arena, node->name);
    node->isExtern = isExtern;
    VecInit(&node->params);

    if (ParserExpect(p, TokLParen, "'('").kind != TokLParen)
    {
        return (Node*)node;
    }

    if (p->m_cur.kind != TokRParen)
    {
        while (true)
        {
            ParamDecl* param = ParseParam(p);

            if (param)
            {
                VecPush(&node->params, param);
            }
            else
            {
                break;
            }

            if (ParserConsume(p, TokComma))
            {
                continue;
            }

            break;
        }
    }

    ParserExpect(p, TokRParen, "')'");

    if (ParserConsume(p, TokSemicolon))
    {
        return (Node*)node;
    }

    if (isExtern)
    {
        DiagError(p->m_diag, p->m_cur.range, "extern function cannot have a body");

        if (p->m_cur.kind == TokLBrace)
        {
            Advance(p);
            Synchronize(p);
        }

        return (Node*)node;
    }

    if (p->m_cur.kind != TokLBrace)
    {
        DiagError(p->m_diag, p->m_cur.range, "expected function body '{...}' or ';'");

        return (Node*)node;
    }

    p->m_returnType = node->returnType.name;
    node->body = ParseBlock(p);
    p->m_returnType = NULL;

    return (Node*)node;
}

static ImportDecl* ParseImport(Parser* p)
{
    Token kw = p->m_cur;
    Advance(p);

    if (p->m_cur.kind != TokIdent)
    {
        DiagErrorFmt(p->m_diag, p->m_cur.range, "expected module path after 'import' but found '%s'", TokSpelling(p->m_cur.kind));
        Synchronize(p);
        return NULL;
    }

    Token first = p->m_cur;
    Token last = first;

    while (p->m_cur.kind == TokIdent || p->m_cur.kind == TokSlash || p->m_cur.kind == TokDot)
    {
        last = p->m_cur;
        Advance(p);
    }

    SourceRange pathRange = SpanFrom(first, last);

    ParserExpect(p, TokSemicolon, "';'");

    ImportDecl* imp = AST_NEW(p->m_arena, ImportDecl);
    imp->base.kind = NodeImport;
    imp->base.range = SpanFrom(kw, last);
    imp->pathRange = pathRange;

    Str src = LexerSourceText(p->m_lex);
    uint32_t pend = SourceRangeEnd(pathRange);
    if (pend > (uint32_t)src.len)
    {
        pend = (uint32_t)src.len;
    }
    Str pathStr = {src.data + pathRange.start, pend - pathRange.start};
    imp->importPath = ToOwned(p->m_arena, pathStr);

    return imp;
}

Module* ParserParseModule(Parser* p)
{
    Module* mod = AST_NEW(p->m_arena, Module);
    mod->base.kind = NodeModule;
    mod->base.range = SRC_INVALID;
    mod->name = p->m_moduleName;
    VecInit(&mod->structs);
    VecInit(&mod->handles);
    VecInit(&mod->functions);
    VecInit(&mod->globals);
    VecInit(&mod->imports);

    while (p->m_cur.kind != TokEof)
    {
        if (p->m_cur.kind == TokSemicolon || p->m_cur.kind == TokRBrace)
        {
            Advance(p);
            continue;
        }

        if (p->m_cur.kind == TokKwImport)
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

            continue;
        }

        if (p->m_cur.kind == TokKwHandle)
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

            continue;
        }

        if (p->m_cur.kind == TokKwStruct)
        {
            StructDecl* sd = ParseStructDecl(p);
            if (sd)
            {
                VecPush(&mod->structs, sd);
            }
            else
            {
                Synchronize(p);
            }

            continue;
        }

        Node* decl = ParseFunction(p);

        if (decl)
        {
            if (decl->kind == NodeFunction)
            {
                VecPush(&mod->functions, decl);
            }
            else if (decl->kind == NodeGlobal)
            {
                VecPush(&mod->globals, decl);
            }
        }
        else
        {
            Synchronize(p);
        }
    }

    return mod;
}

static Node* ParseBlock(Parser* p)
{
    Token lb = ParserExpect(p, TokLBrace, "'{'");

    Block* block = AST_NEW(p->m_arena, Block);
    block->base.kind = NodeBlock;
    block->base.range = lb.range;
    VecInit(&block->statements);

    while (p->m_cur.kind != TokRBrace && p->m_cur.kind != TokEof)
    {
        Node* stmt = ParseStatement(p);

        if (stmt)
        {
            VecPush(&block->statements, stmt);
        }
        else
        {
            Synchronize(p);
        }
    }

    ParserExpect(p, TokRBrace, "'}'");

    return (Node*)block;
}

static Node* ParseVarDeclOrExprStmt(Parser* p);
static Node* ParseReturn(Parser* p);
static Node* ParseIf(Parser* p);
static Node* ParseWhile(Parser* p);
static Node* ParseFor(Parser* p);

static Node* ParseStatement(Parser* p)
{
    switch (p->m_cur.kind)
    {
    case TokLBrace:
        return ParseBlock(p);
    case TokKwReturn:
        return ParseReturn(p);
    case TokKwIf:
        return ParseIf(p);
    case TokKwWhile:
        return ParseWhile(p);
    case TokKwFor:
        return ParseFor(p);
    case TokKwBreak:
    {
        Token token = p->m_cur;

        Advance(p);
        ParserExpect(p, TokSemicolon, "';'");

        BreakStmt* node = AST_NEW(p->m_arena, BreakStmt);
        node->base.kind = NodeBreak;
        node->base.range = token.range;

        return (Node*)node;
    }
    case TokKwContinue:
    {
        Token token = p->m_cur;

        Advance(p);
        ParserExpect(p, TokSemicolon, "';'");

        ContinueStmt* node = AST_NEW(p->m_arena, ContinueStmt);
        node->base.kind = NodeContinue;
        node->base.range = token.range;

        return (Node*)node;
    }
    case TokSemicolon:
    {
        Token token = p->m_cur;

        Advance(p);

        ExprStmt* node = AST_NEW(p->m_arena, ExprStmt);
        node->base.kind = NodeExprStmt;
        node->base.range = token.range;
        node->expr = NULL;

        return (Node*)node;
    }
    default:
        return ParseVarDeclOrExprStmt(p);
    }
}

static Node* ParseVarDeclOrExprStmt(Parser* p)
{
    Token start = p->m_cur;

    if (LooksLikeVarDecl(p))
    {
        TypeName type = {0};

        if (!ParserTryParseType(p, &type))
        {
            return NULL;
        }

        if (p->m_cur.kind != TokIdent)
        {
            DiagError(p->m_diag, p->m_cur.range, "expected a variable name");
            return NULL;
        }

        Token nameTok = p->m_cur;
        Advance(p);

        VarDeclStmt* node = AST_NEW(p->m_arena, VarDeclStmt);
        node->base.kind = NodeVarDecl;
        node->base.range = SpanFrom(start, nameTok);
        node->type = type;
        node->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));

        if (ParserConsume(p, TokAssign))
        {
            if (p->m_cur.kind == TokLBrace)
            {
                node->init = ParseStructInitBody(p, start, type.name);
            }
            else
            {
                node->init = ParseExpr(p);
            }
        }

        ParserExpect(p, TokSemicolon, "';'");

        return (Node*)node;
    }

    Node* e = ParseExpr(p);

    if (!e)
    {
        return NULL;
    }

    ParserExpect(p, TokSemicolon, "';'");

    ExprStmt* node = AST_NEW(p->m_arena, ExprStmt);
    node->base.kind = NodeExprStmt;
    node->base.range = start.range;
    node->expr = e;

    return (Node*)node;
}

static Node* ParseReturn(Parser* p)
{
    Token token = p->m_cur;
    Advance(p);

    ReturnStmt* node = AST_NEW(p->m_arena, ReturnStmt);
    node->base.kind = NodeReturn;
    node->base.range = token.range;

    if (p->m_cur.kind != TokSemicolon)
    {
        if (p->m_cur.kind == TokLBrace && p->m_returnType)
        {
            node->value = ParseStructInitBody(p, p->m_cur, p->m_returnType);
        }
        else
        {
            node->value = ParseExpr(p);
        }
    }

    ParserExpect(p, TokSemicolon, "';'");

    return (Node*)node;
}

static Node* ParseIf(Parser* p)
{
    Token token = p->m_cur;
    Advance(p);

    ParserExpect(p, TokLParen, "'('");
    Node* cond = ParseExpr(p);
    ParserExpect(p, TokRParen, "')'");

    IfStmt* node = AST_NEW(p->m_arena, IfStmt);
    node->base.kind = NodeIf;
    node->base.range = token.range;
    node->condition = cond;
    node->thenBranch = ParseStatement(p);

    if (ParserConsume(p, TokKwElse))
    {
        node->elseBranch = ParseStatement(p);
    }

    return (Node*)node;
}

static Node* ParseWhile(Parser* p)
{
    Token token = p->m_cur;
    Advance(p);

    ParserExpect(p, TokLParen, "'('");
    Node* cond = ParseExpr(p);
    ParserExpect(p, TokRParen, "')'");

    WhileStmt* node = AST_NEW(p->m_arena, WhileStmt);
    node->base.kind = NodeWhile;
    node->base.range = token.range;
    node->condition = cond;
    node->body = ParseStatement(p);

    return (Node*)node;
}

static Node* ParseFor(Parser* p)
{
    Token token = p->m_cur;
    Advance(p);

    if (ParserExpect(p, TokLParen, "'('").kind != TokLParen)
    {
        return NULL;
    }

    Node* init = NULL;

    if (p->m_cur.kind != TokSemicolon)
    {
        if (LooksLikeVarDecl(p))
        {
            TypeName type = {0};

            if (!ParserTryParseType(p, &type))
            {
                return NULL;
            }

            if (p->m_cur.kind != TokIdent)
            {
                DiagError(p->m_diag, p->m_cur.range, "expected a variable name");
                return NULL;
            }

            Token nameTok = p->m_cur;
            Advance(p);

            VarDeclStmt* vd = AST_NEW(p->m_arena, VarDeclStmt);
            vd->base.kind = NodeVarDecl;
            vd->base.range = SpanFrom(token, nameTok);
            vd->type = type;
            vd->name = ToOwned(p->m_arena, ParserIdentText(p, nameTok));

            if (ParserConsume(p, TokAssign))
            {
                vd->init = ParseExpr(p);
            }

            init = (Node*)vd;
        }
        else
        {
            init = ParseExpr(p);
        }
    }

    ParserExpect(p, TokSemicolon, "';'");

    Node* cond = NULL;
    if (p->m_cur.kind != TokSemicolon)
    {
        cond = ParseExpr(p);
    }

    ParserExpect(p, TokSemicolon, "';'");

    Node* update = NULL;
    if (p->m_cur.kind != TokRParen)
    {
        update = ParseExpr(p);
    }

    ParserExpect(p, TokRParen, "')'");

    ForStmt* node = AST_NEW(p->m_arena, ForStmt);
    node->base.kind = NodeFor;
    node->base.range = token.range;
    node->init = init;
    node->condition = cond;
    node->update = update;
    node->body = ParseStatement(p);

    return (Node*)node;
}

static Node* ParseExpr(Parser* p)
{
    return ParseAssign(p);
}

static Node* ParseAssign(Parser* p)
{
    Node* lhs = ParseBinary(p, 0);

    if (IsAssignOp(p->m_cur.kind))
    {
        AssignOp op = MapAssign(p->m_cur.kind);

        Token opToken = p->m_cur;
        Advance(p);

        Node* rhs = ParseAssign(p);

        AssignExpr* node = AST_NEW(p->m_arena, AssignExpr);
        node->base.kind = NodeAssign;
        node->base.range = opToken.range;
        node->op = op;
        node->target = lhs;
        node->value = rhs;

        return (Node*)node;
    }

    return lhs;
}

static Node* ParseBinary(Parser* p, int minPrec)
{
    Node* lhs = ParseUnary(p);

    while (true)
    {
        int prec = 0;
        BinaryOp op;

        if (!BinaryInfo(p->m_cur.kind, &prec, &op))
        {
            break;
        }

        if (prec < minPrec)
        {
            break;
        }

        Advance(p);

        Node* rhs = ParseBinary(p, prec + 1);

        BinaryExpr* node = AST_NEW(p->m_arena, BinaryExpr);
        node->base.kind = NodeBinary;
        node->base.range = lhs->range;
        node->op = op;
        node->lhs = lhs;
        node->rhs = rhs;

        lhs = (Node*)node;
    }

    return lhs;
}

static Node* ParseUnary(Parser* p)
{
    UnaryOp op;

    switch (p->m_cur.kind)
    {
    case TokMinus: op = UnNeg;      break;
    case TokPlus:  op = UnPos;      break;
    case TokBang:  op = UnNot;      break;
    case TokTilde: op = UnBitNot;   break;
    case TokInc:
    case TokDec:
    {
        Token token = p->m_cur;
        Advance(p);

        Node* operand = ParseUnary(p);

        if (!operand)
        {
            return NULL;
        }

        IncDecExpr* node = AST_NEW(p->m_arena, IncDecExpr);
        node->base.kind = NodeIncDec;
        node->base.range = token.range;
        node->isDec = (token.kind == TokDec);
        node->isPrefix = true;
        node->operand = operand;

        return (Node*)node;
    }
    default:
    {
        if (p->m_cur.kind == TokLParen)
        {
            Token next = LexerPeekToken(p->m_lex);

            bool isScalarCast = next.kind == TokKwInt || next.kind == TokKwUint ||
                next.kind == TokKwLong || next.kind == TokKwUlong ||
                next.kind == TokKwFloat || next.kind == TokKwDouble ||
                next.kind == TokKwBool;

            bool isHandleCast = next.kind == TokIdent;

            if (isScalarCast || isHandleCast)
            {
                size_t savedPos = LexerPosition(p->m_lex);
                bool savedPeek = p->m_lex->m_hasPeek;
                Token savedPeeked = p->m_lex->m_peeked;
                Token savedCur = p->m_cur;

                Token lparen = p->m_cur;
                Advance(p);

                TypeName castType = {0};

                if (ParserTryParseType(p, &castType) && p->m_cur.kind == TokRParen)
                {
                    Token afterRparen = LexerPeekToken(p->m_lex);
                    bool startsExpr = afterRparen.kind == TokIdent || afterRparen.kind == TokIntLit ||
                        afterRparen.kind == TokFloatLit || afterRparen.kind == TokBoolLit ||
                        afterRparen.kind == TokLParen || afterRparen.kind == TokMinus ||
                        afterRparen.kind == TokPlus || afterRparen.kind == TokBang ||
                        afterRparen.kind == TokTilde || afterRparen.kind == TokInc ||
                        afterRparen.kind == TokDec;

                    if (startsExpr || isScalarCast)
                    {
                        Advance(p);
                        Node* operand = ParseUnary(p);

                        CastExpr* node = AST_NEW(p->m_arena, CastExpr);
                        node->base.kind = NodeCast;
                        node->base.range = lparen.range;
                        node->type = castType;
                        node->operand = operand;

                        return (Node*)node;
                    }
                }

                p->m_lex->m_pos = savedPos;
                p->m_lex->m_hasPeek = savedPeek;
                p->m_lex->m_peeked = savedPeeked;
                p->m_cur = savedCur;
            }
        }

        return ParsePostfix(p);
    }
    }

    Token token = p->m_cur;
    Advance(p);

    Node* operand = ParseUnary(p);

    if (!operand)
    {
        return NULL;
    }

    UnaryExpr* node = AST_NEW(p->m_arena, UnaryExpr);
    node->base.kind = NodeUnary;
    node->base.range = token.range;
    node->op = op;
    node->operand = operand;

    return (Node*)node;
}

static Node* ParsePostfix(Parser* p)
{
    Node* e = ParsePrimary(p);

    while (e && (p->m_cur.kind == TokDot || p->m_cur.kind == TokInc || p->m_cur.kind == TokDec))
    {
        if (p->m_cur.kind == TokInc || p->m_cur.kind == TokDec)
        {
            Token token = p->m_cur;
            Advance(p);

            IncDecExpr* node = AST_NEW(p->m_arena, IncDecExpr);
            node->base.kind = NodeIncDec;
            node->base.range = token.range;
            node->isDec = (token.kind == TokDec);
            node->isPrefix = false;
            node->operand = e;
            e = (Node*)node;
            continue;
        }

        Token dot = p->m_cur;
        Advance(p);

        if (p->m_cur.kind != TokIdent)
        {
            DiagError(p->m_diag, p->m_cur.range, "expected a member name after '.'");
            break;
        }

        Token memberTok = p->m_cur;
        Advance(p);

        MemberExpr* node = AST_NEW(p->m_arena, MemberExpr);
        node->base.kind = NodeMember;
        node->base.range = SpanFrom(dot, memberTok);
        node->base_node = e;
        node->member = ToOwned(p->m_arena, ParserIdentText(p, memberTok));

        e = (Node*)node;
    }

    return e;
}

static Node* ParseStructInitBody(Parser* p, Token startTok, const char* typeName)
{
    StructInitExpr* init = AST_NEW(p->m_arena, StructInitExpr);
    init->base.kind = NodeStructInit;
    init->base.range = startTok.range;
    init->typeName = arena_strdup(p->m_arena, typeName);
    VecInit(&init->fields);

    Advance(p);

    while (p->m_cur.kind != TokRBrace && p->m_cur.kind != TokEof)
    {
        StructInitField* field = AST_NEW(p->m_arena, StructInitField);

        if (p->m_cur.kind == TokDot)
        {
            Advance(p);

            if (p->m_cur.kind != TokIdent)
            {
                DiagError(p->m_diag, p->m_cur.range, "expected a field name after '.'");
                break;
            }

            Token fieldToken = p->m_cur;
            Advance(p);

            field->name = ToOwned(p->m_arena, ParserIdentText(p, fieldToken));

            ParserExpect(p, TokAssign, "'='");
        }

        field->value = ParseExpr(p);
        VecPush(&init->fields, field);

        if (ParserConsume(p, TokComma))
        {
            continue;
        }

        if (p->m_cur.kind == TokRBrace || p->m_cur.kind == TokEof)
        {
            break;
        }

        DiagError(p->m_diag, p->m_cur.range, "expected ',' or '}' in braced initializer");

        while (p->m_cur.kind != TokComma
            && p->m_cur.kind != TokDot
            && p->m_cur.kind != TokRBrace
            && p->m_cur.kind != TokEof)
        {
            Advance(p);
        }

        ParserConsume(p, TokComma);
    }

    Token close = ParserExpect(p, TokRBrace, "'}'");
    init->base.range = SpanFrom(startTok, close);

    return (Node*)init;
}

static Node* ParsePrimary(Parser* p)
{
    Token token = p->m_cur;

    switch (p->m_cur.kind)
    {
    case TokIntLit:
    {
        Advance(p);

        Str sv = ParserIdentText(p, token);

        bool isUnsigned = sv.len > 0 && (sv.data[sv.len - 1] == 'u' || sv.data[sv.len - 1] == 'U');

        if (isUnsigned)
        {
            sv.len--;
        }

        int base = 10;
        if (sv.len >= 2 && sv.data[0] == '0' && (sv.data[1] == 'x' || sv.data[1] == 'X'))
        {
            base = 16;
        }

        char tmp[64];
        size_t n = sv.len < 63 ? sv.len : 63;
        memcpy(tmp, sv.data, n);
        tmp[n] = '\0';

        uint64_t val = strtoull(tmp, NULL, base);

        IntLiteral* node = AST_NEW(p->m_arena, IntLiteral);
        node->base.kind = NodeIntLiteral;
        node->base.range = token.range;
        node->value = val;
        node->isUnsigned = isUnsigned;

        return (Node*)node;
    }

    case TokFloatLit:
    {
        Advance(p);

        Str sv = ParserIdentText(p, token);

        if (sv.len > 0 && (sv.data[sv.len - 1] == 'f' || sv.data[sv.len - 1] == 'F'))
        {
            sv.len--;
        }

        char tmp[64];
        size_t n = sv.len < 63 ? sv.len : 63;
        memcpy(tmp, sv.data, n);
        tmp[n] = '\0';

        double val = strtod(tmp, NULL);

        FloatLiteral* node = AST_NEW(p->m_arena, FloatLiteral);
        node->base.kind = NodeFloatLiteral;
        node->base.range = token.range;
        node->value = val;

        return (Node*)node;
    }

    case TokBoolLit:
    {
        Advance(p);

        Str sv = ParserIdentText(p, token);
        bool val = StrEqC(sv, "true");

        BoolLiteral* node = AST_NEW(p->m_arena, BoolLiteral);
        node->base.kind = NodeBoolLiteral;
        node->base.range = token.range;
        node->value = val;

        return (Node*)node;
    }

    case TokIdent:
    {
        Advance(p);

        if (p->m_cur.kind == TokLParen)
        {
            CallExpr* call = AST_NEW(p->m_arena, CallExpr);
            call->base.kind = NodeCall;
            call->base.range = token.range;
            call->callee = ToOwned(p->m_arena, ParserIdentText(p, token));
            VecInit(&call->args);

            Advance(p);

            if (p->m_cur.kind != TokRParen)
            {
                while (true)
                {
                    Node* arg = ParseExpr(p);

                    if (arg)
                    {
                        VecPush(&call->args, arg);
                    }

                    if (ParserConsume(p, TokComma))
                    {
                        continue;
                    }

                    break;
                }
            }

            ParserExpect(p, TokRParen, "')'");

            return (Node*)call;
        }

        if (p->m_cur.kind == TokLBrace)
        {
            Str name = ParserIdentText(p, token);

            return ParseStructInitBody(p, token, ToOwned(p->m_arena, name));
        }

        IdentExpr* node = AST_NEW(p->m_arena, IdentExpr);
        node->base.kind = NodeIdent;
        node->base.range = token.range;
        node->name = ToOwned(p->m_arena, ParserIdentText(p, token));

        return (Node*)node;
    }

    case TokLParen:
    {
        Advance(p);

        Node* e = ParseExpr(p);
        ParserExpect(p, TokRParen, "')'");

        return e;
    }

    default:
        DiagErrorFmt(p->m_diag, token.range, "expected an expression but found '%s'", TokSpelling(token.kind));
        return NULL;
    }
}
