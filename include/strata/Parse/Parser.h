// Strata compiler: recursive-descent parser.
//
// Produces a Module AST from a token stream. The grammar is a C-style subset:
//
//   module        := decl*
//   decl          := funcDecl
//   funcDecl      := type ident '(' params? ')' block
//   params        := param (',' param)*
//   param         := paramMod? type ident
//   block         := '{' stmt* '}'
//   stmt          := return | if | while | varDecl | exprStmt | block | break | continue
//   expr          := assignment
//   assignment    := logicOr (assignOp assignment)?
//   logicOr       := logicAnd ('||' logicAnd)*
//   logicAnd      := bitOr ('&&' bitOr)*
//   ... (precedence climbing down to primary)
//   primary       := literal | ident | call | '(' expr ')'
//
// On syntax errors the parser reports a diagnostic and synchronizes to the next
// statement boundary (';' or '}') before continuing, so a single typo yields a
// bounded number of messages.
#pragma once

#include "strata/AST/AST.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Lex/Lexer.h"
#include "strata/Lex/Token.h"

#include <memory>
#include <string>

namespace strata {

class Parser {
public:
    Parser(Lexer& lex, DiagnosticEngine& diag, std::string moduleName = "strata_module")
        : lex_(lex), diag_(diag), moduleName_(std::move(moduleName)) {
        advance();  // prime the current token
    }

    // Parses the whole translation unit. Returns the module even if errors
    // occurred; callers should check diag_.hasErrors() before using it.
    std::unique_ptr<Module> parseModule();

    DiagnosticEngine& diagnostics() noexcept { return diag_; }

private:
    // --- token helpers ---
    const Token& current() const noexcept { return cur_; }
    bool check(TokKind k) const noexcept { return cur_.is(k); }
    bool consume(TokKind k) noexcept;
    Token expect(TokKind k, std::string_view what);
    void advance();
    void synchronize();
    bool looksLikeVarDecl() const noexcept;

    // --- grammar ---
    std::unique_ptr<StructDecl> parseStructDecl();
    std::unique_ptr<HandleDecl> parseHandleDecl();
    std::unique_ptr<FunctionDecl> parseFunction();
    std::unique_ptr<ParamDecl> parseParam();
    NodePtr parseBlock();
    NodePtr parseStatement();
    NodePtr parseVarDeclOrExprStmt();
    NodePtr parseReturn();
    NodePtr parseIf();
    NodePtr parseWhile();
    NodePtr parseFor();

    NodePtr parseExpr();
    NodePtr parseAssign();
    NodePtr parseBinary(int minPrec);
    NodePtr parseUnary();
    NodePtr parsePostfix();
    NodePtr parsePrimary();

    // --- type helpers ---
    bool tryParseType(TypeName& out);
    bool isTypeStart() const noexcept;

    std::string_view identText(const Token& t) const noexcept;

    Lexer& lex_;
    DiagnosticEngine& diag_;
    std::string moduleName_;
    Token cur_{};
};

} // namespace strata
