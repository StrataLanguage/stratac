#include "strata/Parse/Parser.h"

#include "strata/Lex/Token.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

namespace strata {

namespace {

SourceRange spanFrom(const Token& begin, const Token& end) noexcept {
    std::uint32_t s = begin.range.start;
    std::uint32_t e = end.range.end();
    return {s, static_cast<std::uint16_t>(e > s ? e - s : 0)};
}

bool binaryInfo(TokKind k, int& prec, BinaryOp& op) noexcept {
    switch (k) {
        case TokKind::PipePipe: prec = 1; op = BinaryOp::LogicOr;  return true;
        case TokKind::AmpAmp:   prec = 2; op = BinaryOp::LogicAnd; return true;
        case TokKind::Pipe:     prec = 3; op = BinaryOp::BitOr;    return true;
        case TokKind::Caret:    prec = 4; op = BinaryOp::BitXor;   return true;
        case TokKind::Amp:      prec = 5; op = BinaryOp::BitAnd;   return true;
        case TokKind::EqEq:     prec = 6; op = BinaryOp::EqEq;     return true;
        case TokKind::NotEq:    prec = 6; op = BinaryOp::NotEq;    return true;
        case TokKind::Lt:       prec = 7; op = BinaryOp::Lt;       return true;
        case TokKind::LtEq:     prec = 7; op = BinaryOp::LtEq;     return true;
        case TokKind::Gt:       prec = 7; op = BinaryOp::Gt;       return true;
        case TokKind::GtEq:     prec = 7; op = BinaryOp::GtEq;     return true;
        case TokKind::Shl:      prec = 8; op = BinaryOp::Shl;      return true;
        case TokKind::Shr:      prec = 8; op = BinaryOp::Shr;      return true;
        case TokKind::Plus:     prec = 9; op = BinaryOp::Add;      return true;
        case TokKind::Minus:    prec = 9; op = BinaryOp::Sub;      return true;
        case TokKind::Star:     prec = 10; op = BinaryOp::Mul;     return true;
        case TokKind::Slash:    prec = 10; op = BinaryOp::Div;     return true;
        case TokKind::Percent:  prec = 10; op = BinaryOp::Mod;     return true;
        default: return false;
    }
}

AssignOp mapAssign(TokKind k) noexcept {
    switch (k) {
        case TokKind::Assign:   return AssignOp::Assign;
        case TokKind::PlusEq:   return AssignOp::PlusEq;
        case TokKind::MinusEq:  return AssignOp::MinusEq;
        case TokKind::StarEq:   return AssignOp::StarEq;
        case TokKind::SlashEq:  return AssignOp::SlashEq;
        case TokKind::PercentEq:return AssignOp::PercentEq;
        default:                return AssignOp::Assign;
    }
}

bool isAssignOp(TokKind k) noexcept {
    return k == TokKind::Assign || k == TokKind::PlusEq || k == TokKind::MinusEq ||
           k == TokKind::StarEq || k == TokKind::SlashEq || k == TokKind::PercentEq;
}

std::string toString(std::string_view sv) { return std::string(sv); }

} // namespace

std::string_view Parser::identText(const Token& t) const noexcept {
    auto src = lex_.sourceText();
    if (t.range.start >= src.size()) return {};
    auto end = std::min<std::size_t>(t.range.end(), src.size());
    return std::string_view(src.data() + t.range.start, end - t.range.start);
}

void Parser::advance() {
    cur_ = lex_.nextToken();
}

bool Parser::consume(TokKind k) noexcept {
    if (cur_.is(k)) { advance(); return true; }
    return false;
}

Token Parser::expect(TokKind k, std::string_view what) {
    if (cur_.is(k)) {
        Token t = cur_;
        advance();
        return t;
    }
    std::string msg = "expected ";
    msg.append(what);
    msg.append(" but found '");
    msg.append(tokSpelling(cur_.kind));
    msg.append("'");
    diag_.error(cur_.range, std::move(msg));
    return Token{};
}

void Parser::synchronize() {
    while (!cur_.is(TokKind::Eof)) {
        if (cur_.is(TokKind::Semicolon)) { advance(); return; }
        if (cur_.is(TokKind::RBrace)) return;
        advance();
    }
}

bool Parser::looksLikeVarDecl() const noexcept {
    switch (cur_.kind) {
        case TokKind::Kw_const:
        case TokKind::Kw_void:
        case TokKind::Kw_bool:
        case TokKind::Kw_int:
        case TokKind::Kw_uint:
        case TokKind::Kw_half:
        case TokKind::Kw_float:
        case TokKind::Kw_double:
        case TokKind::Kw_string:
            return true;
        case TokKind::Ident: {
            // `float4 x` / `MyStruct s` are var decls; `x + 1` is not.
            Token next = const_cast<Lexer&>(lex_).peekToken();
            return next.is(TokKind::Ident);
        }
        default:
            return false;
    }
}

bool Parser::tryParseType(TypeName& out) {
    bool isConst = false;
    SourceRange constRange = cur_.range;
    if (cur_.is(TokKind::Kw_const)) { isConst = true; advance(); }

    SourceRange r = cur_.range;
    std::string name;
    switch (cur_.kind) {
        case TokKind::Kw_void:   name = "void";   break;
        case TokKind::Kw_bool:   name = "bool";   break;
        case TokKind::Kw_int:    name = "int";    break;
        case TokKind::Kw_uint:   name = "uint";   break;
        case TokKind::Kw_half:   name = "half";   break;
        case TokKind::Kw_float:  name = "float";  break;
        case TokKind::Kw_double: name = "double"; break;
        case TokKind::Kw_string: name = "string"; break;
        case TokKind::Ident:     name = toString(identText(cur_)); break;
        default:
            if (isConst) {
                diag_.error(constRange, "'const' must be followed by a type");
            }
            return false;
    }
    advance();
    out.name = std::move(name);
    out.range = {constRange.start, static_cast<std::uint16_t>(cur_.range.start - constRange.start)};
    out.isConst = isConst;
    (void)r;
    return true;
}

std::unique_ptr<Module> Parser::parseModule() {
    auto mod = std::make_unique<Module>(moduleName_);
    while (!cur_.is(TokKind::Eof)) {
        if (cur_.is(TokKind::Semicolon)) { advance(); continue; } // stray ';'

        // `struct Name {...}` or `extern struct Name;`
        bool startsStruct = false;
        bool externStruct = false;
        if (cur_.is(TokKind::Kw_struct)) {
            startsStruct = true;
        } else if (cur_.is(TokKind::Kw_extern)) {
            Token next = const_cast<Lexer&>(lex_).peekToken();
            if (next.is(TokKind::Kw_struct)) { startsStruct = true; externStruct = true; }
        }

        if (startsStruct) {
            if (externStruct) advance(); // consume `extern`
            auto s = parseStructDecl(externStruct);
            if (s) {
                mod->structs.push_back(std::move(s));
            } else {
                synchronize();
            }
            continue;
        }

        auto fn = parseFunction();
        if (fn) {
            mod->functions.push_back(std::move(fn));
        } else {
            synchronize();
        }
    }
    return mod;
}

std::unique_ptr<StructDecl> Parser::parseStructDecl(bool isExtern) {
    if (!consume(TokKind::Kw_struct)) {
        diag_.error(cur_.range, "expected 'struct'");
        return nullptr;
    }
    if (!cur_.is(TokKind::Ident)) {
        diag_.error(cur_.range, "expected a struct name");
        return nullptr;
    }
    Token nameTok = cur_;
    advance();
    auto s = std::make_unique<StructDecl>(nameTok.range, toString(identText(nameTok)));

    if (consume(TokKind::LBrace)) {
        if (isExtern) {
            diag_.error(nameTok.range, "extern struct cannot have a body (use a plain struct)");
        }
        s->isOpaque = false;
        while (!cur_.is(TokKind::RBrace) && !cur_.is(TokKind::Eof)) {
            TypeName ft;
            if (!tryParseType(ft)) {
                diag_.error(cur_.range, "expected a field type");
                break;
            }
            if (!cur_.is(TokKind::Ident)) {
                diag_.error(cur_.range, "expected a field name");
                break;
            }
            std::string fname = toString(identText(cur_));
            advance();
            s->fields.push_back({std::move(ft), std::move(fname)});
            if (!expect(TokKind::Semicolon, "';'").is(TokKind::Semicolon)) break;
        }
        expect(TokKind::RBrace, "'}'");
    } else {
        s->isOpaque = true; // forward / opaque declaration
    }
    expect(TokKind::Semicolon, "';'");
    return s;
}

std::unique_ptr<FunctionDecl> Parser::parseFunction() {
    bool isExtern = consume(TokKind::Kw_extern);

    TypeName ret;
    if (!tryParseType(ret)) {
        if (!cur_.is(TokKind::Eof)) {
            diag_.error(cur_.range, "expected a type to declare a function");
        }
        return nullptr;
    }
    if (!cur_.is(TokKind::Ident)) {
        diag_.error(cur_.range, "expected a function name");
        return nullptr;
    }
    Token nameTok = cur_;
    advance();
    auto fn = std::make_unique<FunctionDecl>(spanFrom(Token{TokKind::Ident, ret.range}, nameTok),
                                             ret, toString(identText(nameTok)));
    fn->isExtern = isExtern;

    if (!expect(TokKind::LParen, "'('").is(TokKind::LParen)) {
        return fn;
    }
    if (!cur_.is(TokKind::RParen)) {
        while (true) {
            auto param = parseParam();
            if (param) {
                fn->params.push_back(std::move(param));
            } else {
                break;
            }
            if (consume(TokKind::Comma)) continue;
            break;
        }
    }
    expect(TokKind::RParen, "')'");

    if (consume(TokKind::Semicolon)) {
        return fn; // declaration (extern or forward)
    }
    if (isExtern) {
        diag_.error(cur_.range, "extern function cannot have a body");
        // Skip the body if present so parsing can continue.
        if (cur_.is(TokKind::LBrace)) {
            advance();
            synchronize();
        }
        return fn;
    }
    if (!cur_.is(TokKind::LBrace)) {
        diag_.error(cur_.range, "expected function body '{...}' or ';'");
        return fn;
    }
    fn->body = parseBlock();
    return fn;
}

std::unique_ptr<ParamDecl> Parser::parseParam() {
    SourceRange start = cur_.range;
    ParamMod mod = ParamMod::None;
    if (consume(TokKind::Kw_in)) mod = ParamMod::In;
    else if (consume(TokKind::Kw_out)) mod = ParamMod::Out;
    else if (consume(TokKind::Kw_inout)) mod = ParamMod::InOut;

    TypeName type;
    if (!tryParseType(type)) {
        diag_.error(cur_.range, "expected a parameter type");
        return nullptr;
    }
    if (!cur_.is(TokKind::Ident)) {
        diag_.error(cur_.range, "expected a parameter name");
        return nullptr;
    }
    Token nameTok = cur_;
    advance();
    SourceRange r{start.start, static_cast<std::uint16_t>(nameTok.range.end() - start.start)};
    return std::make_unique<ParamDecl>(r, mod, std::move(type), toString(identText(nameTok)));
}

NodePtr Parser::parseBlock() {
    Token lb = expect(TokKind::LBrace, "'{'");
    auto block = std::make_unique<Block>(lb.range);
    while (!cur_.is(TokKind::RBrace) && !cur_.is(TokKind::Eof)) {
        NodePtr s = parseStatement();
        if (s) {
            block->statements.push_back(std::move(s));
        } else {
            synchronize();
        }
    }
    expect(TokKind::RBrace, "'}'");
    return block;
}

NodePtr Parser::parseStatement() {
    switch (cur_.kind) {
        case TokKind::LBrace:     return parseBlock();
        case TokKind::Kw_return:  return parseReturn();
        case TokKind::Kw_if:      return parseIf();
        case TokKind::Kw_while:   return parseWhile();
        case TokKind::Kw_break: {
            Token t = cur_; advance();
            expect(TokKind::Semicolon, "';'");
            return std::make_unique<BreakStmt>(t.range);
        }
        case TokKind::Kw_continue: {
            Token t = cur_; advance();
            expect(TokKind::Semicolon, "';'");
            return std::make_unique<ContinueStmt>(t.range);
        }
        case TokKind::Semicolon: {
            Token t = cur_; advance(); // empty statement
            return std::make_unique<ExprStmt>(t.range, nullptr);
        }
        default:
            return parseVarDeclOrExprStmt();
    }
}

NodePtr Parser::parseVarDeclOrExprStmt() {
    Token start = cur_;
    if (looksLikeVarDecl()) {
        TypeName type;
        if (!tryParseType(type)) return nullptr;
        if (!cur_.is(TokKind::Ident)) {
            diag_.error(cur_.range, "expected a variable name");
            return nullptr;
        }
        Token nameTok = cur_; advance();
        auto vd = std::make_unique<VarDeclStmt>(
            spanFrom(start, nameTok), type, toString(identText(nameTok)));
        if (consume(TokKind::Assign)) {
            vd->init = parseExpr();
        }
        expect(TokKind::Semicolon, "';'");
        return vd;
    }

    NodePtr e = parseExpr();
    if (!e) return nullptr; // let the caller synchronize
    expect(TokKind::Semicolon, "';'");
    return std::make_unique<ExprStmt>(start.range, std::move(e));
}

NodePtr Parser::parseReturn() {
    Token t = cur_; advance(); // 'return'
    auto r = std::make_unique<ReturnStmt>(t.range);
    if (!cur_.is(TokKind::Semicolon)) {
        r->value = parseExpr();
    }
    expect(TokKind::Semicolon, "';'");
    return r;
}

NodePtr Parser::parseIf() {
    Token t = cur_; advance(); // 'if'
    expect(TokKind::LParen, "'('");
    NodePtr cond = parseExpr();
    expect(TokKind::RParen, "')'");
    auto n = std::make_unique<IfStmt>(t.range);
    n->condition = std::move(cond);
    n->thenBranch = parseStatement();
    if (consume(TokKind::Kw_else)) {
        n->elseBranch = parseStatement();
    }
    return n;
}

NodePtr Parser::parseWhile() {
    Token t = cur_; advance(); // 'while'
    expect(TokKind::LParen, "'('");
    NodePtr cond = parseExpr();
    expect(TokKind::RParen, "')'");
    auto n = std::make_unique<WhileStmt>(t.range);
    n->condition = std::move(cond);
    n->body = parseStatement();
    return n;
}

NodePtr Parser::parseExpr() {
    return parseAssign();
}

NodePtr Parser::parseAssign() {
    NodePtr lhs = parseBinary(0);
    if (isAssignOp(cur_.kind)) {
        AssignOp op = mapAssign(cur_.kind);
        Token opTok = cur_; advance();
        NodePtr rhs = parseAssign(); // right-associative
        return std::make_unique<AssignExpr>(
            spanFrom(opTok, opTok), op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

NodePtr Parser::parseBinary(int minPrec) {
    NodePtr lhs = parseUnary();
    while (true) {
        int prec = 0;
        BinaryOp op;
        if (!binaryInfo(cur_.kind, prec, op)) break;
        if (prec < minPrec) break;
        advance();
        NodePtr rhs = parseBinary(prec + 1); // left-associative
        lhs = std::make_unique<BinaryExpr>(Token{TokKind::Unknown, lhs->range}.range,
                                           op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

NodePtr Parser::parseUnary() {
    UnaryOp op;
    switch (cur_.kind) {
        case TokKind::Minus: op = UnaryOp::Neg; break;
        case TokKind::Plus:  op = UnaryOp::Pos; break;
        case TokKind::Bang:  op = UnaryOp::Not; break;
        case TokKind::Tilde: op = UnaryOp::BitNot; break;
        default: return parsePostfix();
    }
    Token t = cur_; advance();
    NodePtr operand = parseUnary();
    if (!operand) return nullptr;
    return std::make_unique<UnaryExpr>(t.range, op, std::move(operand));
}

NodePtr Parser::parsePostfix() {
    NodePtr e = parsePrimary();
    while (e && cur_.is(TokKind::Dot)) {
        Token dot = cur_;
        advance();
        if (!cur_.is(TokKind::Ident)) {
            diag_.error(cur_.range, "expected a member name after '.'");
            break;
        }
        Token memberTok = cur_;
        advance();
        e = std::make_unique<MemberExpr>(spanFrom(dot, memberTok), std::move(e),
                                         toString(identText(memberTok)));
    }
    return e;
}

NodePtr Parser::parsePrimary() {
    Token t = cur_;
    switch (cur_.kind) {
        case TokKind::IntLit: {
            advance();
            auto sv = identText(t);
            bool isUnsigned = !sv.empty() && (sv.back() == 'u' || sv.back() == 'U');
            if (isUnsigned) sv.remove_suffix(1);
            std::uint64_t val = 0;
            int base = 10;
            if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
                base = 16;
            }
            auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val, base);
            if (ec != std::errc()) {
                diag_.error(t.range, "invalid integer literal");
            }
            return std::make_unique<IntLiteral>(t.range, val, isUnsigned);
        }
        case TokKind::FloatLit: {
            advance();
            auto sv = identText(t);
            // strip trailing f/h/F/H suffix
            if (!sv.empty() && (sv.back() == 'f' || sv.back() == 'F' ||
                                sv.back() == 'h' || sv.back() == 'H')) {
                sv.remove_suffix(1);
            }
            std::string tmp(sv);
            double val = std::strtod(tmp.c_str(), nullptr);
            return std::make_unique<FloatLiteral>(t.range, val);
        }
        case TokKind::BoolLit: {
            advance();
            return std::make_unique<BoolLiteral>(t.range, identText(t) == "true");
        }
        case TokKind::Ident: {
            advance();
            if (cur_.is(TokKind::LParen)) {
                auto call = std::make_unique<CallExpr>(t.range, toString(identText(t)));
                advance(); // '('
                if (!cur_.is(TokKind::RParen)) {
                    while (true) {
                        NodePtr arg = parseExpr();
                        if (arg) call->args.push_back(std::move(arg));
                        if (consume(TokKind::Comma)) continue;
                        break;
                    }
                }
                expect(TokKind::RParen, "')'");
                return call;
            }
            return std::make_unique<IdentExpr>(t.range, toString(identText(t)));
        }
        case TokKind::LParen: {
            advance();
            NodePtr e = parseExpr();
            expect(TokKind::RParen, "')'");
            return e;
        }
        default:
            diag_.error(t.range, std::string("expected an expression but found '") +
                                   std::string(tokSpelling(t.kind)) + "'");
            return nullptr;
    }
}

} // namespace strata
