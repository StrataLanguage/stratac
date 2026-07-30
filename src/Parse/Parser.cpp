#include "strata/Parse/Parser.h"

#include "strata/Lex/Token.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

namespace strata
{

namespace
{

SourceRange SpanFrom(const Token& begin, const Token& end) noexcept
{
    std::uint32_t s = begin.range.start;
    std::uint32_t e = end.range.End();
    return {
        .start = s,
        .length = static_cast<std::uint16_t>(e > s ? e - s : 0),
    };
}

bool BinaryInfo(TokKind k, int& prec, BinaryOp& op) noexcept
{
    switch (k)
    {
    case TokKind::PipePipe:
        prec = 1;
        op = BinaryOp::LogicOr;
        return true;
    case TokKind::AmpAmp:
        prec = 2;
        op = BinaryOp::LogicAnd;
        return true;
    case TokKind::Pipe:
        prec = 3;
        op = BinaryOp::BitOr;
        return true;
    case TokKind::Caret:
        prec = 4;
        op = BinaryOp::BitXor;
        return true;
    case TokKind::Amp:
        prec = 5;
        op = BinaryOp::BitAnd;
        return true;
    case TokKind::EqEq:
        prec = 6;
        op = BinaryOp::EqEq;
        return true;
    case TokKind::NotEq:
        prec = 6;
        op = BinaryOp::NotEq;
        return true;
    case TokKind::Lt:
        prec = 7;
        op = BinaryOp::Lt;
        return true;
    case TokKind::LtEq:
        prec = 7;
        op = BinaryOp::LtEq;
        return true;
    case TokKind::Gt:
        prec = 7;
        op = BinaryOp::Gt;
        return true;
    case TokKind::GtEq:
        prec = 7;
        op = BinaryOp::GtEq;
        return true;
    case TokKind::Shl:
        prec = 8;
        op = BinaryOp::Shl;
        return true;
    case TokKind::Shr:
        prec = 8;
        op = BinaryOp::Shr;
        return true;
    case TokKind::Plus:
        prec = 9;
        op = BinaryOp::Add;
        return true;
    case TokKind::Minus:
        prec = 9;
        op = BinaryOp::Sub;
        return true;
    case TokKind::Star:
        prec = 10;
        op = BinaryOp::Mul;
        return true;
    case TokKind::Slash:
        prec = 10;
        op = BinaryOp::Div;
        return true;
    case TokKind::Percent:
        prec = 10;
        op = BinaryOp::Mod;
        return true;
    default:
        return false;
    }
}

AssignOp MapAssign(TokKind k) noexcept
{
    switch (k)
    {
    case TokKind::Assign:
        return AssignOp::Assign;
    case TokKind::PlusEq:
        return AssignOp::PlusEq;
    case TokKind::MinusEq:
        return AssignOp::MinusEq;
    case TokKind::StarEq:
        return AssignOp::StarEq;
    case TokKind::SlashEq:
        return AssignOp::SlashEq;
    case TokKind::PercentEq:
        return AssignOp::PercentEq;
    default:
        return AssignOp::Assign;
    }
}

bool IsAssignOp(TokKind k) noexcept
{
    return k == TokKind::Assign || k == TokKind::PlusEq || k == TokKind::MinusEq || k == TokKind::StarEq ||
           k == TokKind::SlashEq || k == TokKind::PercentEq;
}

std::string ToString(std::string_view stringView)
{
    return std::string(stringView);
}

} // namespace

std::string_view Parser::IdentText(const Token& t) const noexcept
{
    auto src = m_lex.SourceText();
    if (t.range.start >= src.size())
    {
        return {};
    }

    auto end = std::min<std::size_t>(t.range.End(), src.size());
    return std::string_view(src.data() + t.range.start, end - t.range.start);
}

void Parser::Advance()
{
    m_cur = m_lex.NextToken();
}

bool Parser::Consume(TokKind k) noexcept
{
    if (m_cur.Is(k))
    {
        Advance();
        return true;
    }

    return false;
}

Token Parser::Expect(TokKind k, std::string_view what)
{
    if (m_cur.Is(k))
    {
        Token token = m_cur;
        Advance();
        return token;
    }

    std::string msg = "expected ";
    msg.append(what);
    msg.append(" but found '");
    msg.append(TokSpelling(m_cur.kind));
    msg.append("'");
    m_diag.Error(m_cur.range, std::move(msg));
    return Token{};
}

void Parser::Synchronize()
{
    while (!m_cur.Is(TokKind::Eof))
    {
        if (m_cur.Is(TokKind::Semicolon))
        {
            Advance();
            return;
        }

        if (m_cur.Is(TokKind::RBrace))
        {
            return;
        }

        Advance();
    }
}

bool Parser::LooksLikeVarDecl() const noexcept
{
    switch (m_cur.kind)
    {
    case TokKind::KwConst:
    case TokKind::KwVoid:
    case TokKind::KwBool:
    case TokKind::KwUint:
    case TokKind::KwInt:
    case TokKind::KwHalf:
    case TokKind::KwFloat:
    case TokKind::KwDouble:
    case TokKind::KwString:
        return true;
    case TokKind::Ident:
    {
        // `float4 x` / `MyStruct s` are var decls; `x + 1` is not.
        Token next = const_cast<Lexer&>(m_lex).PeekToken();
        return next.Is(TokKind::Ident);
    }

    default:
        return false;
    }
}

bool Parser::TryParseType(TypeName& out)
{
    bool isConst = false;
    SourceRange constRange = m_cur.range;
    if (m_cur.Is(TokKind::KwConst))
    {
        isConst = true;
        Advance();
    }

    SourceRange range = m_cur.range;
    std::string name;
    switch (m_cur.kind)
    {
    case TokKind::KwVoid:
        name = "void";
        break;
    case TokKind::KwBool:
        name = "bool";
        break;
    case TokKind::KwInt:
        name = "int";
        break;
    case TokKind::KwUint:
        name = "uint";
        break;
    case TokKind::KwHalf:
        name = "half";
        break;
    case TokKind::KwFloat:
        name = "float";
        break;
    case TokKind::KwDouble:
        name = "double";
        break;
    case TokKind::KwString:
        name = "string";
        break;
    case TokKind::Ident:
        name = ToString(IdentText(m_cur));
        break;
    default:
        if (isConst)
        {
            m_diag.Error(constRange, "'const' must be followed by a type");
        }

        return false;
    }

    Advance();
    out.name = std::move(name);
    out.range = {
        .start = constRange.start,
        .length = static_cast<std::uint16_t>(m_cur.range.start - constRange.start),
    };
    out.isConst = isConst;
    (void)range;
    return true;
}

std::unique_ptr<Module> Parser::ParseModule()
{
    auto mod = std::make_unique<Module>(m_moduleName);
    while (!m_cur.Is(TokKind::Eof))
    {
        if (m_cur.Is(TokKind::Semicolon))
        {
            Advance();
            continue;
        } // stray ';'

        if (m_cur.Is(TokKind::KwHandle))
        {
            auto handleDecl = ParseHandleDecl();
            if (handleDecl)
            {
                mod->handles.push_back(std::move(handleDecl));
            }
            else
            {
                Synchronize();
            }

            continue;
        }

        if (m_cur.Is(TokKind::KwStruct))
        {
            auto structDecl = ParseStructDecl();
            if (structDecl)
            {
                mod->structs.push_back(std::move(structDecl));
            }
            else
            {
                Synchronize();
            }

            continue;
        }

        // `extern` is a function linkage marker. Catch the old `extern struct`
        // form and point users at `handle`/`struct`.
        if (m_cur.Is(TokKind::KwExtern))
        {
            Token next = m_lex.PeekToken();
            if (next.Is(TokKind::KwStruct) || next.Is(TokKind::KwHandle))
            {
                m_diag.Error(m_cur.range, "'extern' applies to functions; declare an opaque type with "
                                          "'handle Name;' or a value type with 'struct Name { ... }'");
                Advance(); // consume extern
                Synchronize();
                continue;
            }
        }

        auto functionDecl = ParseFunction();
        if (functionDecl)
        {
            mod->functions.push_back(std::move(functionDecl));
        }
        else
        {
            Synchronize();
        }
    }

    return mod;
}

std::unique_ptr<HandleDecl> Parser::ParseHandleDecl()
{
    if (!Consume(TokKind::KwHandle))
    {
        m_diag.Error(m_cur.range, "expected 'handle'");
        return nullptr;
    }

    if (!m_cur.Is(TokKind::Ident))
    {
        m_diag.Error(m_cur.range, "expected a handle name");
        return nullptr;
    }

    Token nameTok = m_cur;
    Advance();
    auto handleDecl = std::make_unique<HandleDecl>(nameTok.range, ToString(IdentText(nameTok)));
    if (Consume(TokKind::LBrace))
    {
        m_diag.Error(nameTok.range, "handle '" + handleDecl->name +
                                        "' cannot have a body (it is opaque; use 'struct' for a value type)");
        Synchronize();
    }

    Expect(TokKind::Semicolon, "';'");
    return handleDecl;
}

std::unique_ptr<StructDecl> Parser::ParseStructDecl()
{
    if (!Consume(TokKind::KwStruct))
    {
        m_diag.Error(m_cur.range, "expected 'struct'");
        return nullptr;
    }

    if (!m_cur.Is(TokKind::Ident))
    {
        m_diag.Error(m_cur.range, "expected a struct name");
        return nullptr;
    }

    Token nameTok = m_cur;
    Advance();
    auto structDecl = std::make_unique<StructDecl>(nameTok.range, ToString(IdentText(nameTok)));

    if (Consume(TokKind::LBrace))
    {
        while (!m_cur.Is(TokKind::RBrace) && !m_cur.Is(TokKind::Eof))
        {
            TypeName ft;
            if (!TryParseType(ft))
            {
                m_diag.Error(m_cur.range, "expected a field type");
                break;
            }

            if (!m_cur.Is(TokKind::Ident))
            {
                m_diag.Error(m_cur.range, "expected a field name");
                break;
            }

            std::string fname = ToString(IdentText(m_cur));
            Advance();
            structDecl->fields.push_back({std::move(ft), std::move(fname)});
            if (!Expect(TokKind::Semicolon, "';'").Is(TokKind::Semicolon))
            {
                break;
            }
        }

        Expect(TokKind::RBrace, "'}'");
    }
    else
    {
        m_diag.Error(nameTok.range, "struct '" + structDecl->name + "' requires a body; use 'handle " +
                                        structDecl->name + ";' for an opaque type");
    }

    Expect(TokKind::Semicolon, "';'");
    return structDecl;
}

std::unique_ptr<FunctionDecl> Parser::ParseFunction()
{
    bool isExtern = Consume(TokKind::KwExtern);

    TypeName returnType;
    if (!TryParseType(returnType))
    {
        if (!m_cur.Is(TokKind::Eof))
        {
            m_diag.Error(m_cur.range, "expected a type to declare a function");
        }

        return nullptr;
    }

    if (!m_cur.Is(TokKind::Ident))
    {
        m_diag.Error(m_cur.range, "expected a function name");
        return nullptr;
    }

    Token nameTok = m_cur;
    Advance();
    auto functionDecl = std::make_unique<FunctionDecl>(SpanFrom(Token{TokKind::Ident, returnType.range}, nameTok),
                                                       returnType, ToString(IdentText(nameTok)));
    functionDecl->isExtern = isExtern;

    if (!Expect(TokKind::LParen, "'('").Is(TokKind::LParen))
    {
        return functionDecl;
    }

    if (!m_cur.Is(TokKind::RParen))
    {
        while (true)
        {
            auto param = ParseParam();
            if (param)
            {
                functionDecl->params.push_back(std::move(param));
            }
            else
            {
                break;
            }

            if (Consume(TokKind::Comma))
            {
                continue;
            }

            break;
        }
    }

    Expect(TokKind::RParen, "')'");

    if (Consume(TokKind::Semicolon))
    {
        return functionDecl; // declaration (extern or forward)
    }

    if (isExtern)
    {
        m_diag.Error(m_cur.range, "extern function cannot have a body");
        // Skip the body if present so parsing can continue.
        if (m_cur.Is(TokKind::LBrace))
        {
            Advance();
            Synchronize();
        }

        return functionDecl;
    }

    if (!m_cur.Is(TokKind::LBrace))
    {
        m_diag.Error(m_cur.range, "expected function body '{...}' or ';'");
        return functionDecl;
    }

    functionDecl->body = ParseBlock();
    return functionDecl;
}

std::unique_ptr<ParamDecl> Parser::ParseParam()
{
    SourceRange start = m_cur.range;
    ParamMod mod = ParamMod::None;
    if (Consume(TokKind::KwIn))
    {
        mod = ParamMod::In;
    }
    else if (Consume(TokKind::KwOut))
    {
        mod = ParamMod::Out;
    }
    else if (Consume(TokKind::KwInout))
    {
        mod = ParamMod::InOut;
    }

    TypeName type;
    if (!TryParseType(type))
    {
        m_diag.Error(m_cur.range, "expected a parameter type");
        return nullptr;
    }

    if (!m_cur.Is(TokKind::Ident))
    {
        m_diag.Error(m_cur.range, "expected a parameter name");
        return nullptr;
    }

    Token nameTok = m_cur;
    Advance();
    SourceRange range{
        .start = start.start,
        .length = static_cast<std::uint16_t>(nameTok.range.End() - start.start),
    };
    return std::make_unique<ParamDecl>(range, mod, std::move(type), ToString(IdentText(nameTok)));
}

NodePtr Parser::ParseBlock()
{
    Token lb = Expect(TokKind::LBrace, "'{'");
    auto block = std::make_unique<Block>(lb.range);
    while (!m_cur.Is(TokKind::RBrace) && !m_cur.Is(TokKind::Eof))
    {
        NodePtr stmt = ParseStatement();
        if (stmt)
        {
            block->statements.push_back(std::move(stmt));
        }
        else
        {
            Synchronize();
        }
    }

    Expect(TokKind::RBrace, "'}'");
    return block;
}

NodePtr Parser::ParseStatement()
{
    switch (m_cur.kind)
    {
    case TokKind::LBrace:
        return ParseBlock();
    case TokKind::KwReturn:
        return ParseReturn();
    case TokKind::KwIf:
        return ParseIf();
    case TokKind::KwWhile:
        return ParseWhile();
    case TokKind::KwFor:
        return ParseFor();
    case TokKind::KwBreak:
    {
        Token token = m_cur;
        Advance();
        Expect(TokKind::Semicolon, "';'");
        return std::make_unique<BreakStmt>(token.range);
    }

    case TokKind::KwContinue:
    {
        Token token = m_cur;
        Advance();
        Expect(TokKind::Semicolon, "';'");
        return std::make_unique<ContinueStmt>(token.range);
    }

    case TokKind::Semicolon:
    {
        Token token = m_cur;
        Advance(); // empty statement
        return std::make_unique<ExprStmt>(token.range, nullptr);
    }

    default:
        return ParseVarDeclOrExprStmt();
    }
}

NodePtr Parser::ParseVarDeclOrExprStmt()
{
    Token start = m_cur;
    if (LooksLikeVarDecl())
    {
        TypeName type;
        if (!TryParseType(type))
        {
            return nullptr;
        }

        if (!m_cur.Is(TokKind::Ident))
        {
            m_diag.Error(m_cur.range, "expected a variable name");
            return nullptr;
        }

        Token nameTok = m_cur;
        Advance();
        auto varDecl = std::make_unique<VarDeclStmt>(SpanFrom(start, nameTok), type, ToString(IdentText(nameTok)));
        if (Consume(TokKind::Assign))
        {
            varDecl->init = ParseExpr();
        }

        Expect(TokKind::Semicolon, "';'");
        return varDecl;
    }

    NodePtr e = ParseExpr();
    if (!e)
    {
        return nullptr; // let the caller synchronize
    }

    Expect(TokKind::Semicolon, "';'");
    return std::make_unique<ExprStmt>(start.range, std::move(e));
}

NodePtr Parser::ParseReturn()
{
    Token token = m_cur;
    Advance(); // 'return'
    auto r = std::make_unique<ReturnStmt>(token.range);
    if (!m_cur.Is(TokKind::Semicolon))
    {
        r->value = ParseExpr();
    }

    Expect(TokKind::Semicolon, "';'");
    return r;
}

NodePtr Parser::ParseIf()
{
    Token token = m_cur;
    Advance(); // 'if'
    Expect(TokKind::LParen, "'('");
    NodePtr cond = ParseExpr();
    Expect(TokKind::RParen, "')'");
    auto node = std::make_unique<IfStmt>(token.range);
    node->condition = std::move(cond);
    node->thenBranch = ParseStatement();
    if (Consume(TokKind::KwElse))
    {
        node->elseBranch = ParseStatement();
    }

    return node;
}

NodePtr Parser::ParseWhile()
{
    Token token = m_cur;
    Advance(); // 'while'
    Expect(TokKind::LParen, "'('");
    NodePtr cond = ParseExpr();
    Expect(TokKind::RParen, "')'");
    auto node = std::make_unique<WhileStmt>(token.range);
    node->condition = std::move(cond);
    node->body = ParseStatement();
    return node;
}

NodePtr Parser::ParseFor()
{
    Token token = m_cur;
    Advance(); // 'for'
    if (!Expect(TokKind::LParen, "'('").Is(TokKind::LParen))
    {
        return nullptr;
    }

    // init: optional var decl or expression
    NodePtr init;
    if (!m_cur.Is(TokKind::Semicolon))
    {
        if (LooksLikeVarDecl())
        {
            TypeName type;
            if (!TryParseType(type))
            {
                return nullptr;
            }

            if (!m_cur.Is(TokKind::Ident))
            {
                m_diag.Error(m_cur.range, "expected a variable name");
                return nullptr;
            }

            Token nameTok = m_cur;
            Advance();
            auto varDecl = std::make_unique<VarDeclStmt>(SpanFrom(token, nameTok), type, ToString(IdentText(nameTok)));
            if (Consume(TokKind::Assign))
            {
                varDecl->init = ParseExpr();
            }

            init = std::move(varDecl);
        }
        else
        {
            init = ParseExpr();
        }
    }

    Expect(TokKind::Semicolon, "';'");

    NodePtr cond;
    if (!m_cur.Is(TokKind::Semicolon))
    {
        cond = ParseExpr();
    }

    Expect(TokKind::Semicolon, "';'");

    NodePtr update;
    if (!m_cur.Is(TokKind::RParen))
    {
        update = ParseExpr();
    }

    Expect(TokKind::RParen, "')'");

    auto node = std::make_unique<ForStmt>(token.range);
    node->init = std::move(init);
    node->condition = std::move(cond);
    node->update = std::move(update);
    node->body = ParseStatement();
    return node;
}

NodePtr Parser::ParseExpr()
{
    return ParseAssign();
}

NodePtr Parser::ParseAssign()
{
    NodePtr lhs = ParseBinary(0);
    if (IsAssignOp(m_cur.kind))
    {
        AssignOp op = MapAssign(m_cur.kind);
        Token opTok = m_cur;
        Advance();
        NodePtr rhs = ParseAssign(); // right-associative
        return std::make_unique<AssignExpr>(SpanFrom(opTok, opTok), op, std::move(lhs), std::move(rhs));
    }

    return lhs;
}

NodePtr Parser::ParseBinary(int minPrec)
{
    NodePtr lhs = ParseUnary();
    while (true)
    {
        int prec = 0;
        BinaryOp op;
        if (!BinaryInfo(m_cur.kind, prec, op))
        {
            break;
        }

        if (prec < minPrec)
        {
            break;
        }

        Advance();
        NodePtr rhs = ParseBinary(prec + 1); // left-associative
        lhs =
            std::make_unique<BinaryExpr>(Token{TokKind::Unknown, lhs->range}.range, op, std::move(lhs), std::move(rhs));
    }

    return lhs;
}

NodePtr Parser::ParseUnary()
{
    UnaryOp op;
    switch (m_cur.kind)
    {
    case TokKind::Minus:
        op = UnaryOp::Neg;
        break;
    case TokKind::Plus:
        op = UnaryOp::Pos;
        break;
    case TokKind::Bang:
        op = UnaryOp::Not;
        break;
    case TokKind::Tilde:
        op = UnaryOp::BitNot;
        break;
    default:
        return ParsePostfix();
    }

    Token token = m_cur;
    Advance();
    NodePtr operand = ParseUnary();
    if (!operand)
    {
        return nullptr;
    }

    return std::make_unique<UnaryExpr>(token.range, op, std::move(operand));
}

NodePtr Parser::ParsePostfix()
{
    NodePtr e = ParsePrimary();
    while (e && m_cur.Is(TokKind::Dot))
    {
        Token dot = m_cur;
        Advance();
        if (!m_cur.Is(TokKind::Ident))
        {
            m_diag.Error(m_cur.range, "expected a member name after '.'");
            break;
        }

        Token memberTok = m_cur;
        Advance();
        e = std::make_unique<MemberExpr>(SpanFrom(dot, memberTok), std::move(e), ToString(IdentText(memberTok)));
    }

    return e;
}

NodePtr Parser::ParsePrimary()
{
    Token token = m_cur;
    switch (m_cur.kind)
    {
    case TokKind::IntLit:
    {
        Advance();
        auto stringView = IdentText(token);
        bool isUnsigned = !stringView.empty() && (stringView.back() == 'u' || stringView.back() == 'U');
        if (isUnsigned)
        {
            stringView.remove_suffix(1);
        }

        std::uint64_t val = 0;
        int base = 10;
        if (stringView.size() >= 2 && stringView[0] == '0' && (stringView[1] == 'x' || stringView[1] == 'X'))
        {
            base = 16;
        }

        auto [position, errorCode] =
            std::from_chars(stringView.data(), stringView.data() + stringView.size(), val, base);
        if (errorCode != std::errc())
        {
            m_diag.Error(token.range, "invalid integer literal");
        }

        return std::make_unique<IntLiteral>(token.range, val, isUnsigned);
    }

    case TokKind::FloatLit:
    {
        Advance();
        auto stringView = IdentText(token);
        // strip trailing f/h/F/H suffix
        if (!stringView.empty() && (stringView.back() == 'f' || stringView.back() == 'F' || stringView.back() == 'h' ||
                                    stringView.back() == 'H'))
        {
            stringView.remove_suffix(1);
        }

        std::string tmp(stringView);
        double val = std::strtod(tmp.c_str(), nullptr);
        return std::make_unique<FloatLiteral>(token.range, val);
    }

    case TokKind::BoolLit:
    {
        Advance();
        return std::make_unique<BoolLiteral>(token.range, IdentText(token) == "true");
    }

    case TokKind::Ident:
    {
        Advance();
        if (m_cur.Is(TokKind::LParen))
        {
            auto call = std::make_unique<CallExpr>(token.range, ToString(IdentText(token)));
            Advance(); // '('
            if (!m_cur.Is(TokKind::RParen))
            {
                while (true)
                {
                    NodePtr arg = ParseExpr();
                    if (arg)
                    {
                        call->args.push_back(std::move(arg));
                    }

                    if (Consume(TokKind::Comma))
                    {
                        continue;
                    }

                    break;
                }
            }

            Expect(TokKind::RParen, "')'");
            return call;
        }

        return std::make_unique<IdentExpr>(token.range, ToString(IdentText(token)));
    }

    case TokKind::LParen:
    {
        Advance();
        NodePtr e = ParseExpr();
        Expect(TokKind::RParen, "')'");
        return e;
    }

    default:
        m_diag.Error(token.range,
                     std::string("expected an expression but found '") + std::string(TokSpelling(token.kind)) + "'");
        return nullptr;
    }
}

} // namespace strata
