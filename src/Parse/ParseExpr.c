#include "Parse/ParserInternal.h"

#include "Codegen/TypeRegistry.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Node* ParseAssign(Parser* p);
static Node* ParseUnary(Parser* p);
static Node* ParsePostfix(Parser* p);
static void ParseCallArgs(Parser* p, CallExpr* call);
static Node* ParsePrimary(Parser* p);

static bool BinaryInfo(TokKind k, int* prec, BinaryOp* op)
{
    switch (k)
    {
    case TokPipePipe:
        *prec = 1;
        *op = BinLogicOr;
        return true;
    case TokAmpAmp:
        *prec = 2;
        *op = BinLogicAnd;
        return true;
    case TokPipe:
        *prec = 3;
        *op = BinBitOr;
        return true;
    case TokCaret:
        *prec = 4;
        *op = BinBitXor;
        return true;
    case TokAmp:
        *prec = 5;
        *op = BinBitAnd;
        return true;
    case TokEqEq:
        *prec = 6;
        *op = BinEqEq;
        return true;
    case TokNotEq:
        *prec = 6;
        *op = BinNotEq;
        return true;
    case TokLt:
        *prec = 7;
        *op = BinLt;
        return true;
    case TokLtEq:
        *prec = 7;
        *op = BinLtEq;
        return true;
    case TokGt:
        *prec = 7;
        *op = BinGt;
        return true;
    case TokGtEq:
        *prec = 7;
        *op = BinGtEq;
        return true;
    case TokShl:
        *prec = 8;
        *op = BinShl;
        return true;
    case TokShr:
        *prec = 8;
        *op = BinShr;
        return true;
    case TokPlus:
        *prec = 9;
        *op = BinAdd;
        return true;
    case TokMinus:
        *prec = 9;
        *op = BinSub;
        return true;
    case TokStar:
        *prec = 10;
        *op = BinMul;
        return true;
    case TokSlash:
        *prec = 10;
        *op = BinDiv;
        return true;
    case TokPercent:
        *prec = 10;
        *op = BinMod;
        return true;
    default:
        return false;
    }
}

static AssignOp MapAssign(TokKind k)
{
    switch (k)
    {
    case TokAssign:
        return AssignSet;
    case TokPlusEq:
        return AssignAdd;
    case TokMinusEq:
        return AssignSub;
    case TokStarEq:
        return AssignMul;
    case TokSlashEq:
        return AssignDiv;
    case TokPercentEq:
        return AssignMod;
    default:
        return AssignSet;
    }
}

static bool IsAssignOp(TokKind k)
{
    return k == TokAssign || k == TokPlusEq || k == TokMinusEq || k == TokStarEq || k == TokSlashEq
           || k == TokPercentEq;
}

char* ToOwned(Arena* arena, Str s)
{
    return arena_strndup(arena, s.data, s.len);
}

Node* ParseExpr(Parser* p)
{
    return ParseAssign(p);
}

/* Tokens that can begin an expression: after `?` they make it the conditional operator rather than
   the postfix null test (`opt?` is followed by `)`, `;`, `&&`, `||`, ...). */
static bool StartsExpression(TokKind k)
{
    switch (k)
    {
    case TokIdent:
    case TokIntLit:
    case TokFloatLit:
    case TokBoolLit:
    case TokStrLit:
    case TokCharLit:
    case TokKwTrue:
    case TokKwFalse:
    case TokLParen:
    case TokBang:
    case TokTilde:
    case TokMinus:
    case TokKwFloat2:
    case TokKwFloat3:
    case TokKwFloat4:
        return true;
    default:
        return false;
    }
}

/* `cond ? a : b` (right-associative, binds looser than every binary operator). */
static Node* ParseConditional(Parser* p)
{
    Node* cond = ParseBinary(p, 0);

    if (!cond || p->m_cur.kind != TokQuestion)
    {
        return cond;
    }

    Token q = p->m_cur;
    Advance(p);

    Node* thenExpr = ParseExpr(p);
    ParserExpect(p, TokColon, "':'");
    Node* elseExpr = ParseConditional(p);

    ConditionalExpr* node = AST_NEW(p->m_arena, ConditionalExpr);
    node->base.kind = NodeConditional;
    node->base.range = q.range;
    node->condition = cond;
    node->thenExpr = thenExpr;
    node->elseExpr = elseExpr;

    return (Node*)node;
}

static Node* ParseAssign(Parser* p)
{
    Node* lhs = ParseConditional(p);

    if (!lhs)
    {
        return NULL;
    }

    if (IsAssignOp(p->m_cur.kind))
    {
        AssignOp op = MapAssign(p->m_cur.kind);

        Token opToken = p->m_cur;
        Advance(p);

        // Bare braces are array literals; `{ .f = ... }` is a struct literal.
        Node* rhs;

        if (p->m_cur.kind == TokLBrace)
        {
            if (LexerPeekToken(p->m_lex).kind == TokDot)
            {
                rhs = ParseStructInitBody(p, p->m_cur, NULL);
            }
            else
            {
                rhs = ParseArrayInitBody(p, p->m_cur, NULL);
            }
        }
        else
        {
            rhs = ParseAssign(p);
        }

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

Node* ParseBinary(Parser* p, int minPrec)
{
    Node* lhs = ParseUnary(p);

    if (!lhs)
    {
        return NULL;
    }

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

        if (!rhs)
        {
            AstDispose(lhs);
            return NULL;
        }

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
    case TokMinus:
        op = UnNeg;
        break;
    case TokPlus:
        op = UnPos;
        break;
    case TokBang:
        op = UnNot;
        break;
    case TokTilde:
        op = UnBitNot;
        break;
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

            bool isScalarCast = next.kind == TokKwInt || next.kind == TokKwUint || next.kind == TokKwLong
                                || next.kind == TokKwUlong || next.kind == TokKwByte || next.kind == TokKwSbyte
                                || next.kind == TokKwShort || next.kind == TokKwUshort || next.kind == TokKwFloat
                                || next.kind == TokKwDouble || next.kind == TokKwBool || next.kind == TokKwString
                                || next.kind == TokKwCstring;

            bool isHandleCast = next.kind == TokIdent;
            bool isBoxCast = next.kind == TokCaret;

            if (isScalarCast || isHandleCast || isBoxCast)
            {
                LexerCheckpoint saved = SaveLexerState(p);

                Token lparen = p->m_cur;
                Advance(p);

                TypeName castType = {0};

                if (ParserTryParseType(p, &castType) && p->m_cur.kind == TokRParen)
                {
                    /* The parser has no type table (user types may be declared
                       later or imported), so `(Name)` is a cast only when the
                       next token can start a cast operand but cannot continue
                       an expression - the C# rule. `-`, `+`, `++` and `--`
                       could be either (`(x) - 1`, `(x)++`), so they parse as
                       an expression; write `(Name)(-x)` to cast a negation.
                       Keyword and box casts are unambiguous. */
                    Token afterRparen = LexerPeekToken(p->m_lex);
                    bool startsOperand = afterRparen.kind == TokIdent || afterRparen.kind == TokIntLit
                                         || afterRparen.kind == TokFloatLit || afterRparen.kind == TokBoolLit
                                         || afterRparen.kind == TokStrLit || afterRparen.kind == TokCharLit
                                         || afterRparen.kind == TokLParen || afterRparen.kind == TokBang
                                         || afterRparen.kind == TokTilde || afterRparen.kind == TokKwInt
                                         || afterRparen.kind == TokKwUint || afterRparen.kind == TokKwLong
                                         || afterRparen.kind == TokKwUlong || afterRparen.kind == TokKwByte
                                         || afterRparen.kind == TokKwSbyte || afterRparen.kind == TokKwShort
                                         || afterRparen.kind == TokKwUshort || afterRparen.kind == TokKwFloat
                                         || afterRparen.kind == TokKwDouble || afterRparen.kind == TokKwFloat2
                                         || afterRparen.kind == TokKwFloat3 || afterRparen.kind == TokKwFloat4;

                    if (startsOperand || isScalarCast || isBoxCast)
                    {
                        Advance(p);
                        Node* operand = ParseUnary(p);

                        if (!operand)
                        {
                            return NULL;
                        }

                        CastExpr* node = AST_NEW(p->m_arena, CastExpr);
                        node->base.kind = NodeCast;
                        node->base.range = lparen.range;
                        node->type = castType;
                        node->operand = operand;

                        return (Node*)node;
                    }
                }

                RestoreLexerState(p, saved);
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

    while (e
           && (p->m_cur.kind == TokDot || p->m_cur.kind == TokInc || p->m_cur.kind == TokDec
               || p->m_cur.kind == TokLBracket || p->m_cur.kind == TokQuestion))
    {
        if (p->m_cur.kind == TokQuestion && StartsExpression(LexerPeekToken(p->m_lex).kind))
        {
            break; // `cond ? a : b` — ParseConditional takes the `?`
        }

        if (p->m_cur.kind == TokQuestion)
        {
            // `expr?` — null test (postfix). Only meaningful on a `T?`; sema rejects it everywhere else.
            Token q = p->m_cur;
            Advance(p);

            NullTestExpr* node = AST_NEW(p->m_arena, NullTestExpr);
            node->base.kind = NodeNullTest;
            node->base.range = q.range;
            node->operand = e;

            e = (Node*)node;
            continue;
        }

        if (p->m_cur.kind == TokLBracket)
        {
            Token lbr = p->m_cur;
            Advance(p);

            Node* index = ParseExpr(p);
            Token close = ParserExpect(p, TokRBracket, "']'");

            if (!index)
            {
                AstDispose(e);
                return NULL;
            }

            IndexExpr* node = AST_NEW(p->m_arena, IndexExpr);
            node->base.kind = NodeIndex;
            node->base.range = SpanFrom(lbr, close);
            node->base_node = e;
            node->index = index;

            e = (Node*)node;
            continue;
        }

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

        /* `ptr.null`, the null pointer, is the one constant read off a type name. */
        if (e->kind == NodeIdent && strcmp(((IdentExpr*)e)->name, STRATA_PTR_TYPE) == 0
            && StrEqC(ParserIdentText(p, memberTok), "null") && p->m_cur.kind != TokLParen)
        {
            IdentExpr* constant = AST_NEW(p->m_arena, IdentExpr);
            constant->base.kind = NodeIdent;
            uint32_t end = SourceRangeEnd(memberTok.range);
            constant->base.range = (SourceRange){e->range.start, end - e->range.start, e->range.fileId};
            constant->name = (char*)STRATA_NULL_CONSTANT;
            e = (Node*)constant;
            continue;
        }

        MemberExpr* node = AST_NEW(p->m_arena, MemberExpr);
        node->base.kind = NodeMember;
        node->base.range = SpanFrom(dot, memberTok);
        node->base_node = e;
        node->member = ToOwned(p->m_arena, ParserIdentText(p, memberTok));

        e = (Node*)node;

        /* `expr.Member(args)` — a method call. The callee keeps the member
           name; sema resolves it against impl blocks (prepending the base as
           the self argument for instance methods). */
        if (p->m_cur.kind == TokLParen)
        {
            CallExpr* call = AST_NEW(p->m_arena, CallExpr);
            call->base.kind = NodeCall;
            call->base.range = node->base.range;
            call->callee = node->member;
            call->calleeBase = node->base_node;
            call->isIntrinsicCall = false;
            VecInit(&call->args);

            ParseCallArgs(p, call);
            call->base.range = SpanFrom(dot, p->m_cur);

            e = (Node*)call;
        }
    }

    return e;
}

/* Parses `(arg, ...)` after the opening paren has NOT yet been consumed.
   Shared by plain calls (ParsePrimary) and member calls (ParsePostfix). */
static void ParseCallArgs(Parser* p, CallExpr* call)
{
    Advance(p); /* '(' */

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
}

Node* ParseStructInitBody(Parser* p, Token startTok, const char* typeName)
{
    StructInitExpr* init = AST_NEW(p->m_arena, StructInitExpr);
    init->base.kind = NodeStructInit;
    init->base.range = startTok.range;
    init->typeName = typeName ? arena_strdup(p->m_arena, typeName) : NULL;
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

        if (p->m_cur.kind == TokLBrace)
        {
            /* `{ ... }` initializes a fixed-size array field (element type
               filled from the field in sema); a designator form is a nested
               struct literal. */
            if (LexerPeekToken(p->m_lex).kind == TokDot)
            {
                field->value = ParseStructInitBody(p, p->m_cur, NULL);
            }
            else
            {
                field->value = ParseArrayInitBody(p, p->m_cur, NULL);
            }
        }
        else
        {
            field->value = ParseExpr(p);
        }

        if (field->value)
        {
            VecPush(&init->fields, field);
        }

        if (ParserConsume(p, TokComma))
        {
            continue;
        }

        if (p->m_cur.kind == TokRBrace || p->m_cur.kind == TokEof)
        {
            break;
        }

        DiagError(p->m_diag, p->m_cur.range, "expected ',' or '}' in braced initializer");

        while (p->m_cur.kind != TokComma && p->m_cur.kind != TokDot && p->m_cur.kind != TokRBrace
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

Node* ParseArrayInitBody(Parser* p, Token startTok, const TypeName* elementType)
{
    ArrayInitExpr* init = AST_NEW(p->m_arena, ArrayInitExpr);
    init->base.kind = NodeArrayInit;
    init->base.range = startTok.range;
    init->elementType = elementType;
    init->repeatCount = NULL;
    VecInit(&init->elements);

    Advance(p); /* consume '{' */

    while (p->m_cur.kind != TokRBrace && p->m_cur.kind != TokEof)
    {
        Node* elem = ParseExpr(p);

        if (elem)
        {
            VecPush(&init->elements, elem);
        }

        /* `{ value; count }`: count copies of one value. */
        if (init->elements.count == 1 && ParserConsume(p, TokSemicolon))
        {
            init->repeatCount = ParseExpr(p);
            break;
        }

        if (ParserConsume(p, TokComma))
        {
            continue;
        }

        if (p->m_cur.kind == TokRBrace || p->m_cur.kind == TokEof)
        {
            break;
        }

        DiagError(p->m_diag, p->m_cur.range, "expected ',' or '}' in array initializer");

        while (p->m_cur.kind != TokComma && p->m_cur.kind != TokRBrace && p->m_cur.kind != TokEof)
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

        bool isUnsigned = false;
        bool isLong = false;
        uint64_t val = IntLiteralValueEx(p, token, &isUnsigned, &isLong);

        IntLiteral* node = AST_NEW(p->m_arena, IntLiteral);
        node->base.kind = NodeIntLiteral;
        node->base.range = token.range;
        node->value = val;
        node->isUnsigned = isUnsigned;
        node->isLong = isLong;

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

        // Full spelling (no fixed-size buffer), so long literals are never truncated.
        char* text = arena_strndup(p->m_arena, sv.data, sv.len);
        double val = strtod(text, NULL);

        if (isinf(val))
        {
            DiagError(p->m_diag, token.range, "floating-point literal is out of range");
            val = 0.0;
        }

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

    case TokCharLit:
    {
        Advance(p);

        /* `'c'` is the byte constant `(byte)N` — the same nodes as the cast, so it goes wherever that
           does (comparisons, arithmetic, fills, enum values, global initializers). The lexer has
           already diagnosed a malformed literal; whatever byte it starts with stands in. */
        Str sv = ParserIdentText(p, token);
        unsigned char value = sv.len > 1 ? (unsigned char)sv.data[1] : 0;

        if (value == '\\' && sv.len > 2)
        {
            switch (sv.data[2])
            {
            case 'n':
                value = '\n';
                break;
            case 't':
                value = '\t';
                break;
            case 'r':
                value = '\r';
                break;
            case '0':
                value = '\0';
                break;
            default:
                value = (unsigned char)sv.data[2];
                break; /* \\ \' \" */
            }
        }

        IntLiteral* lit = AST_NEW(p->m_arena, IntLiteral);
        lit->base.kind = NodeIntLiteral;
        lit->base.range = token.range;
        lit->value = value;

        CastExpr* node = AST_NEW(p->m_arena, CastExpr);
        node->base.kind = NodeCast;
        node->base.range = token.range;
        node->type.name = "byte";
        node->type.primitiveType = GetPrimitiveType("byte");
        node->type.range = token.range;
        node->operand = (Node*)lit;

        return (Node*)node;
    }

    case TokStrLit:
    {
        Advance(p);

        Str sv = ParserIdentText(p, token);

        if (sv.len < 2)
        {
            StrLiteral* node = AST_NEW(p->m_arena, StrLiteral);
            node->base.kind = NodeStrLiteral;
            node->base.range = token.range;
            node->value = arena_strndup(p->m_arena, "", 0);
            node->length = 0;

            return (Node*)node;
        }

        char* raw = arena_strndup(p->m_arena, sv.data + 1, sv.len - 2);

        char* dst = raw;
        for (size_t i = 0; i < sv.len - 2; i++)
        {
            if (raw[i] == '\\' && i + 1 < sv.len - 2)
            {
                char next = raw[i + 1];
                switch (next)
                {
                case '\\':
                    *dst++ = '\\';
                    i++;
                    break;
                case '"':
                    *dst++ = '"';
                    i++;
                    break;
                case 'n':
                    *dst++ = '\n';
                    i++;
                    break;
                case 't':
                    *dst++ = '\t';
                    i++;
                    break;
                case 'r':
                    *dst++ = '\r';
                    i++;
                    break;
                case '0':
                    *dst++ = '\0';
                    i++;
                    break;
                default:
                    *dst++ = raw[i];
                    break;
                }
            }
            else
            {
                *dst++ = raw[i];
            }
        }
        *dst = '\0';

        StrLiteral* node = AST_NEW(p->m_arena, StrLiteral);
        node->base.kind = NodeStrLiteral;
        node->base.range = token.range;
        node->value = raw;
        node->length = (size_t)(dst - raw); // a `\0` escape embeds a NUL, so strlen(value) may be shorter

        return (Node*)node;
    }

    case TokKwFloat2:
    case TokKwFloat3:
    case TokKwFloat4:
    case TokKwInt:
    case TokKwUint:
    case TokKwLong:
    case TokKwUlong:
    case TokKwByte:
    case TokKwSbyte:
    case TokKwShort:
    case TokKwUshort:
    case TokKwFloat:
    case TokKwDouble:
    case TokIdent:
    {
        Advance(p);

        /* `Name<Type>(args)`: an explicit type argument for a generic extern. Anything
           else starting `Name <` is a comparison, so the attempt is rolled back. */
        if (token.kind == TokIdent && p->m_cur.kind == TokLt)
        {
            LexerCheckpoint saved = SaveLexerState(p);
            Advance(p);

            TypeName typeArg = {0};

            if (ParserTryParseType(p, &typeArg) && typeArg.name && p->m_cur.kind == TokGt)
            {
                Advance(p);

                if (p->m_cur.kind == TokLParen)
                {
                    CallExpr* call = AST_NEW(p->m_arena, CallExpr);
                    call->base.kind = NodeCall;
                    call->base.range = token.range;
                    call->callee = ToOwned(p->m_arena, ParserIdentText(p, token));
                    call->isIntrinsicCall = false;
                    call->typeArg = (TypeName*)arena_dup(p->m_arena, &typeArg, sizeof(TypeName));
                    VecInit(&call->args);

                    ParseCallArgs(p, call);

                    return (Node*)call;
                }
            }

            RestoreLexerState(p, saved);
        }

        if (p->m_cur.kind == TokLParen)
        {
            CallExpr* call = AST_NEW(p->m_arena, CallExpr);
            call->base.kind = NodeCall;
            call->base.range = token.range;
            call->callee = ToOwned(p->m_arena, ParserIdentText(p, token));
            call->isIntrinsicCall = false;
            VecInit(&call->args);

            ParseCallArgs(p, call);

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

    case TokLBrace:
    {
        /* A braced list in expression position: `{1,2,3}` as an array literal
           or `{ .field = ... }` as a struct literal (types inferred in sema).
           Statement position never reaches here — a leading `{` starts a block. */
        if (LexerPeekToken(p->m_lex).kind == TokDot)
        {
            return ParseStructInitBody(p, token, NULL);
        }

        return ParseArrayInitBody(p, token, NULL);
    }

    default:
        DiagErrorFmt(p->m_diag, token.range, "expected an expression but found '%s'", TokSpelling(token.kind));
        return NULL;
    }
}
