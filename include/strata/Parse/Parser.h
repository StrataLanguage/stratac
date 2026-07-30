#pragma once

#include "strata/AST/AST.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Lex/Lexer.h"
#include "strata/Lex/Token.h"

#include <memory>
#include <string>

namespace strata
{

class Parser
{
  public:
    Parser(Lexer& lex, DiagnosticEngine& diag, std::string moduleName = "strata_module")
        : m_lex(lex), m_diag(diag), m_moduleName(std::move(moduleName))
    {
        Advance(); // prime the current token
    }

    // Parses the whole translation unit. Returns the module even if errors
    // occurred; callers should check diag_.hasErrors() before using it.
    std::unique_ptr<Module> ParseModule();

    DiagnosticEngine& Diagnostics() noexcept
    {
        return m_diag;
    }

  private:
    // --- token helpers ---
    const Token& Current() const noexcept
    {
        return m_cur;
    }

    bool Check(TokKind k) const noexcept
    {
        return m_cur.Is(k);
    }

    bool Consume(TokKind k) noexcept;
    Token Expect(TokKind k, std::string_view what);
    void Advance();
    void Synchronize();
    bool LooksLikeVarDecl() const noexcept;

    // --- grammar ---
    std::unique_ptr<StructDecl> ParseStructDecl();
    std::unique_ptr<HandleDecl> ParseHandleDecl();
    std::unique_ptr<FunctionDecl> ParseFunction();
    std::unique_ptr<ParamDecl> ParseParam();
    NodePtr ParseBlock();
    NodePtr ParseStatement();
    NodePtr ParseVarDeclOrExprStmt();
    NodePtr ParseReturn();
    NodePtr ParseIf();
    NodePtr ParseWhile();
    NodePtr ParseFor();

    NodePtr ParseExpr();
    NodePtr ParseAssign();
    NodePtr ParseBinary(int minPrec);
    NodePtr ParseUnary();
    NodePtr ParsePostfix();
    NodePtr ParsePrimary();

    // Parses the body of a braced struct initializer: '{' fields '}'.
    // `startTok` anchors the source range; `typeName` is the struct type.
    NodePtr ParseStructInitBody(const Token& startTok, std::string typeName);

    // --- type helpers ---
    bool TryParseType(TypeName& out);
    bool IsTypeStart() const noexcept;

    std::string_view IdentText(const Token& t) const noexcept;

    Lexer& m_lex;
    DiagnosticEngine& m_diag;
    std::string m_moduleName;
    Token m_cur;
};

} // namespace strata
