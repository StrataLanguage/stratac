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
    return {.start = s, .length = static_cast<std::uint16_t>(e > s ? e - s : 0)};
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

std::string ToString(std::string_view sv)
{
    return std::string(sv);
}

} // namespace

std::string_view Parser::IdentText(const Token& t) const noexcept
{
    auto src = m_lex.SourceText();
    if (t.range.start >= src.size()) return {};
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
        Token t = m_cur;
        Advance();
        return t;
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
        if (m_cur.Is(TokKind::RBrace)) return;
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

    SourceRange r = m_cur.range;
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
    out.range = {.start = constRange.start, .length = static_cast<std::uint16_t>(m_cur.range.start - constRange.start)};
    out.isConst = isConst;
    (void)r;
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
            auto h = ParseHandleDecl();
            if (h)
            {
                mod->handles.push_back(std::move(h));
            }
            else
            {
                Synchronize();
            }
            continue;
        }
        if (m_cur.Is(TokKind::KwStruct))
        {
            auto s = ParseStructDecl();
            if (s)
            {
                mod->structs.push_back(std::move(s));
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

        auto fn = ParseFunction();
        if (fn)
        {
            mod->functions.push_back(std::move(fn));
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
    auto h = std::make_unique<HandleDecl>(nameTok.range, ToString(IdentText(nameTok)));
    if (Consume(TokKind::LBrace))
    {
        m_diag.Error(nameTok.range,
                     "handle '" + h->name + "' cannot have a body (it is opaque; use 'struct' for a value type)");
        Synchronize();
    }
    Expect(TokKind::Semicolon, "';'");
    return h;
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
    auto s = std::make_unique<StructDecl>(nameTok.range, ToString(IdentText(nameTok)));

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
            s->fields.push_back({std::move(ft), std::move(fname)});
            if (!Expect(TokKind::Semicolon, "';'").Is(TokKind::Semicolon)) break;
        }
        Expect(TokKind::RBrace, "'}'");
    }
    else
    {
        m_diag.Error(nameTok.range,
                     "struct '" + s->name + "' requires a body; use 'handle " + s->name + ";' for an opaque type");
    }
    Expect(TokKind::Semicolon, "';'");
    return s;
}

std::unique_ptr<FunctionDecl> Parser::ParseFunction()
{
    bool isExtern = Consume(TokKind::KwExtern);

    TypeName ret;
    if (!TryParseType(ret))
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
    auto fn = std::make_unique<FunctionDecl>(SpanFrom(Token{TokKind::Ident, ret.range}, nameTok), ret,
                                             ToString(IdentText(nameTok)));
    fn->isExtern = isExtern;

    if (!Expect(TokKind::LParen, "'('").Is(TokKind::LParen))
    {
        return fn;
    }
    if (!m_cur.Is(TokKind::RParen))
    {
        while (true)
        {
            auto param = ParseParam();
            if (param)
            {
                fn->params.push_back(std::move(param));
            }
            else
            {
                break;
            }
            if (Consume(TokKind::Comma)) continue;
            break;
        }
    }
    Expect(TokKind::RParen, "')'");

    if (Consume(TokKind::Semicolon))
    {
        return fn; // declaration (extern or forward)
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
        return fn;
    }
    if (!m_cur.Is(TokKind::LBrace))
    {
        m_diag.Error(m_cur.range, "expected function body '{...}' or ';'");
        return fn;
    }
    fn->body = ParseBlock();
    return fn;
}

std::unique_ptr<ParamDecl> Parser::ParseParam()
{
    SourceRange start = m_cur.range;
    ParamMod mod = ParamMod::None;
    if (Consume(TokKind::KwIn))
        mod = ParamMod::In;
    else if (Consume(TokKind::KwOut))
        mod = ParamMod::Out;
    else if (Consume(TokKind::KwInout))
        mod = ParamMod::InOut;

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
    SourceRange r{.start = start.start, .length = static_cast<std::uint16_t>(nameTok.range.End() - start.start)};
    return std::make_unique<ParamDecl>(r, mod, std::move(type), ToString(IdentText(nameTok)));
}

NodePtr Parser::ParseBlock()
{
    Token lb = Expect(TokKind::LBrace, "'{'");
    auto block = std::make_unique<Block>(lb.range);
    while (!m_cur.Is(TokKind::RBrace) && !m_cur.Is(TokKind::Eof))
    {
        NodePtr s = ParseStatement();
        if (s)
        {
            block->statements.push_back(std::move(s));
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
        Token t = m_cur;
        Advance();
        Expect(TokKind::Semicolon, "';'");
        return std::make_unique<BreakStmt>(t.range);
    }
    case TokKind::KwContinue:
    {
        Token t = m_cur;
        Advance();
        Expect(TokKind::Semicolon, "';'");
        return std::make_unique<ContinueStmt>(t.range);
    }
    case TokKind::Semicolon:
    {
        Token t = m_cur;
        Advance(); // empty statement
        return std::make_unique<ExprStmt>(t.range, nullptr);
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
        if (!TryParseType(type)) return nullptr;
        if (!m_cur.Is(TokKind::Ident))
        {
            m_diag.Error(m_cur.range, "expected a variable name");
            return nullptr;
        }
        Token nameTok = m_cur;
        Advance();
        auto vd = std::make_unique<VarDeclStmt>(SpanFrom(start, nameTok), type, ToString(IdentText(nameTok)));
        if (Consume(TokKind::Assign))
        {
            vd->init = ParseExpr();
        }
        Expect(TokKind::Semicolon, "';'");
        return vd;
    }

    NodePtr e = ParseExpr();
    if (!e) return nullptr; // let the caller synchronize
    Expect(TokKind::Semicolon, "';'");
    return std::make_unique<ExprStmt>(start.range, std::move(e));
}

NodePtr Parser::ParseReturn()
{
    Token t = m_cur;
    Advance(); // 'return'
    auto r = std::make_unique<ReturnStmt>(t.range);
    if (!m_cur.Is(TokKind::Semicolon))
    {
        r->value = ParseExpr();
    }
    Expect(TokKind::Semicolon, "';'");
    return r;
}

NodePtr Parser::ParseIf()
{
    Token t = m_cur;
    Advance(); // 'if'
    Expect(TokKind::LParen, "'('");
    NodePtr cond = ParseExpr();
    Expect(TokKind::RParen, "')'");
    auto n = std::make_unique<IfStmt>(t.range);
    n->condition = std::move(cond);
    n->thenBranch = ParseStatement();
    if (Consume(TokKind::KwElse))
    {
        n->elseBranch = ParseStatement();
    }
    return n;
}

NodePtr Parser::ParseWhile()
{
    Token t = m_cur;
    Advance(); // 'while'
    Expect(TokKind::LParen, "'('");
    NodePtr cond = ParseExpr();
    Expect(TokKind::RParen, "')'");
    auto n = std::make_unique<WhileStmt>(t.range);
    n->condition = std::move(cond);
    n->body = ParseStatement();
    return n;
}

NodePtr Parser::ParseFor()
{
    Token t = m_cur;
    Advance(); // 'for'
    if (!Expect(TokKind::LParen, "'('").Is(TokKind::LParen)) return nullptr;

    // init: optional var decl or expression
    NodePtr init;
    if (!m_cur.Is(TokKind::Semicolon))
    {
        if (LooksLikeVarDecl())
        {
            TypeName type;
            if (!TryParseType(type)) return nullptr;
            if (!m_cur.Is(TokKind::Ident))
            {
                m_diag.Error(m_cur.range, "expected a variable name");
                return nullptr;
            }
            Token nameTok = m_cur;
            Advance();
            auto vd = std::make_unique<VarDeclStmt>(SpanFrom(t, nameTok), type, ToString(IdentText(nameTok)));
            if (Consume(TokKind::Assign)) vd->init = ParseExpr();
            init = std::move(vd);
        }
        else
        {
            init = ParseExpr();
        }
    }
    Expect(TokKind::Semicolon, "';'");

    NodePtr cond;
    if (!m_cur.Is(TokKind::Semicolon)) cond = ParseExpr();
    Expect(TokKind::Semicolon, "';'");

    NodePtr update;
    if (!m_cur.Is(TokKind::RParen)) update = ParseExpr();
    Expect(TokKind::RParen, "')'");

    auto n = std::make_unique<ForStmt>(t.range);
    n->init = std::move(init);
    n->condition = std::move(cond);
    n->update = std::move(update);
    n->body = ParseStatement();
    return n;
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
        if (!BinaryInfo(m_cur.kind, prec, op)) break;
        if (prec < minPrec) break;
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
    Token t = m_cur;
    Advance();
    NodePtr operand = ParseUnary();
    if (!operand) return nullptr;
    return std::make_unique<UnaryExpr>(t.range, op, std::move(operand));
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
    Token t = m_cur;
    switch (m_cur.kind)
    {
    case TokKind::IntLit:
    {
        Advance();
        auto sv = IdentText(t);
        bool isUnsigned = !sv.empty() && (sv.back() == 'u' || sv.back() == 'U');
        if (isUnsigned) sv.remove_suffix(1);
        std::uint64_t val = 0;
        int base = 10;
        if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X'))
        {
            base = 16;
        }
        auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val, base);
        if (ec != std::errc())
        {
            m_diag.Error(t.range, "invalid integer literal");
        }
        return std::make_unique<IntLiteral>(t.range, val, isUnsigned);
    }
    case TokKind::FloatLit:
    {
        Advance();
        auto sv = IdentText(t);
        // strip trailing f/h/F/H suffix
        if (!sv.empty() && (sv.back() == 'f' || sv.back() == 'F' || sv.back() == 'h' || sv.back() == 'H'))
        {
            sv.remove_suffix(1);
        }
        std::string tmp(sv);
        double val = std::strtod(tmp.c_str(), nullptr);
        return std::make_unique<FloatLiteral>(t.range, val);
    }
    case TokKind::BoolLit:
    {
        Advance();
        return std::make_unique<BoolLiteral>(t.range, IdentText(t) == "true");
    }
    case TokKind::Ident:
    {
        Advance();
        if (m_cur.Is(TokKind::LParen))
        {
            auto call = std::make_unique<CallExpr>(t.range, ToString(IdentText(t)));
            Advance(); // '('
            if (!m_cur.Is(TokKind::RParen))
            {
                while (true)
                {
                    NodePtr arg = ParseExpr();
                    if (arg) call->args.push_back(std::move(arg));
                    if (Consume(TokKind::Comma)) continue;
                    break;
                }
            }
            Expect(TokKind::RParen, "')'");
            return call;
        }
        return std::make_unique<IdentExpr>(t.range, ToString(IdentText(t)));
    }
    case TokKind::LParen:
    {
        Advance();
        NodePtr e = ParseExpr();
        Expect(TokKind::RParen, "')'");
        return e;
    }
    default:
        m_diag.Error(t.range,
                     std::string("expected an expression but found '") + std::string(TokSpelling(t.kind)) + "'");
        return nullptr;
    }
}

} // namespace strata
