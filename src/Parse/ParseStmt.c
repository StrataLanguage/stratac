#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Node* ParseStatement(Parser* p);
static Node* ParseVarDeclOrExprStmt(Parser* p);
static Node* ParseReturn(Parser* p);
static Node* ParseIf(Parser* p);
static Node* ParseWhile(Parser* p);
static Node* ParseFor(Parser* p);

Node* ParseBlock(Parser* p)
{
    Token lb = ParserExpect(p, TokLBrace, "'{'");

    Block* block = AST_NEW(p->arena, Block);
    block->base.kind = NodeBlock;
    block->base.range = lb.range;
    VecInit(&block->statements);

    while (p->cur.kind != TokRBrace && p->cur.kind != TokEof)
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

static Node* ParseStatement(Parser* p)
{
    switch (p->cur.kind)
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
    case TokKwDefer:
    {
        Token token = p->cur;
        Advance(p);

        Node* stmt = ParseStatement(p);

        if (!stmt || (stmt->kind == NodeExprStmt && ((ExprStmt*)stmt)->expr == NULL))
        {
            DiagError(p->diag, token.range, "expected a statement after 'defer'");
            return NULL;
        }

        DeferStmt* node = AST_NEW(p->arena, DeferStmt);
        node->base.kind = NodeDefer;
        node->base.range = token.range;
        node->stmt = stmt;

        return (Node*)node;
    }
    case TokKwBreak:
    {
        Token token = p->cur;

        Advance(p);
        ParserExpect(p, TokSemicolon, "';'");

        BreakStmt* node = AST_NEW(p->arena, BreakStmt);
        node->base.kind = NodeBreak;
        node->base.range = token.range;

        return (Node*)node;
    }
    case TokKwContinue:
    {
        Token token = p->cur;

        Advance(p);
        ParserExpect(p, TokSemicolon, "';'");

        ContinueStmt* node = AST_NEW(p->arena, ContinueStmt);
        node->base.kind = NodeContinue;
        node->base.range = token.range;

        return (Node*)node;
    }
    case TokSemicolon:
    {
        Token token = p->cur;

        Advance(p);

        ExprStmt* node = AST_NEW(p->arena, ExprStmt);
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
    Token start = p->cur;

    if (LooksLikeVarDecl(p))
    {
        TypeName type = {0};

        /* `ref T x = path;` and `const ref T x = path;` bind an alias. */
        bool isRef = false;
        bool refConst = false;

        if (p->cur.kind == TokKwConst)
        {
            LexerCheckpoint saved = SaveLexerState(p);
            Advance(p);

            if (p->cur.kind == TokKwRef)
            {
                refConst = true;
            }
            else
            {
                RestoreLexerState(p, saved);
            }
        }

        if (ParserConsume(p, TokKwRef))
        {
            isRef = true;
        }

        if (!ParserTryParseType(p, &type))
        {
            return NULL;
        }

        if (refConst)
        {
            type.isConst = true;
        }

        if (p->cur.kind != TokIdent)
        {
            DiagError(p->diag, p->cur.range, "expected a variable name");
            return NULL;
        }

        Token nameTok = p->cur;
        Advance(p);

        VarDeclStmt* node = AST_NEW(p->arena, VarDeclStmt);
        node->base.kind = NodeVarDecl;
        node->base.range = SpanFrom(start, nameTok);
        node->type = type;
        node->name = ToOwned(p->arena, ParserIdentText(p, nameTok));
        node->isRef = isRef;
        node->refThroughBox = false;

        if (ParserConsume(p, TokAssign))
        {
            if (p->cur.kind == TokLBrace)
            {
                if (type.isArray)
                {
                    node->init = ParseArrayInitBody(p, start, type.elem);
                }
                else if (type.isOptional && type.inner && type.inner->isArray)
                {
                    // `T[]? x = { ... };` - a braced literal initializes the inner dynamic array.
                    node->init = ParseArrayInitBody(p, start, type.inner->elem);
                }
                else
                {
                    // Box inits infer the inner T.
                    const char* initTypeName = NULL;

                    if ((type.isBox || type.isOptional) && type.inner)
                    {
                        initTypeName = type.inner->name;
                    }
                    else
                    {
                        initTypeName = type.name;

                        if (type.primitiveType == PrimString)
                        {
                            initTypeName = NULL;
                        }
                    }

                    node->init = ParseStructInitBody(p, start, initTypeName);
                }
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

    ExprStmt* node = AST_NEW(p->arena, ExprStmt);
    node->base.kind = NodeExprStmt;
    node->base.range = start.range;
    node->expr = e;

    return (Node*)node;
}

static Node* ParseReturn(Parser* p)
{
    Token token = p->cur;
    Advance(p);

    p->hasReturnStmt = true;

    ReturnStmt* node = AST_NEW(p->arena, ReturnStmt);
    node->base.kind = NodeReturn;
    node->base.range = token.range;

    if (p->cur.kind != TokSemicolon)
    {
        if (p->cur.kind == TokLBrace && p->returnType)
        {
            const TypeName* rtArr = p->returnType->isOptional ? p->returnType->inner : p->returnType;

            if (TypeNameIsDynamicArray(rtArr))
            {
                node->value = ParseArrayInitBody(p, p->cur, rtArr->elem);
            }
            else
            {
                /* `return {};` through a plain, box, or optional return type
                   initializes the INNER struct (E, never "E?" / "^E"). */
                const TypeName* rtStruct = rtArr && rtArr->isBox ? rtArr->inner : rtArr;

                node->value = ParseStructInitBody(p, p->cur, rtStruct ? rtStruct->name : rtArr->name);
            }
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
    Token token = p->cur;
    Advance(p);

    ParserExpect(p, TokLParen, "'('");
    Node* cond = ParseExpr(p);
    ParserExpect(p, TokRParen, "')'");

    IfStmt* node = AST_NEW(p->arena, IfStmt);
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
    Token token = p->cur;
    Advance(p);

    ParserExpect(p, TokLParen, "'('");
    Node* cond = ParseExpr(p);
    ParserExpect(p, TokRParen, "')'");

    WhileStmt* node = AST_NEW(p->arena, WhileStmt);
    node->base.kind = NodeWhile;
    node->base.range = token.range;
    node->condition = cond;
    node->body = ParseStatement(p);

    return (Node*)node;
}

static Node* ParseFor(Parser* p)
{
    Token token = p->cur;
    Advance(p);

    if (ParserExpect(p, TokLParen, "'('").kind != TokLParen)
    {
        return NULL;
    }

    Node* init = NULL;

    if (p->cur.kind != TokSemicolon)
    {
        if (LooksLikeVarDecl(p))
        {
            TypeName type = {0};

            if (!ParserTryParseType(p, &type))
            {
                return NULL;
            }

            if (p->cur.kind != TokIdent)
            {
                DiagError(p->diag, p->cur.range, "expected a variable name");
                return NULL;
            }

            Token nameTok = p->cur;
            Advance(p);

            VarDeclStmt* vd = AST_NEW(p->arena, VarDeclStmt);
            vd->base.kind = NodeVarDecl;
            vd->base.range = SpanFrom(token, nameTok);
            vd->type = type;
            vd->name = ToOwned(p->arena, ParserIdentText(p, nameTok));

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
    if (p->cur.kind != TokSemicolon)
    {
        cond = ParseExpr(p);
    }

    ParserExpect(p, TokSemicolon, "';'");

    Node* update = NULL;
    if (p->cur.kind != TokRParen)
    {
        update = ParseExpr(p);
    }

    ParserExpect(p, TokRParen, "')'");

    ForStmt* node = AST_NEW(p->arena, ForStmt);
    node->base.kind = NodeFor;
    node->base.range = token.range;
    node->init = init;
    node->condition = cond;
    node->update = update;
    node->body = ParseStatement(p);

    return (Node*)node;
}
