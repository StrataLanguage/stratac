#pragma once

#include "strata/AST/AST.h"
#include "strata/Core/Diagnostics.h"
#include "strata/Lex/Lexer.h"
#include "strata/Lex/Token.h"
#include "strata/Parse/Parser.h"
#include "strata/Sema/ResolveOverloads.h"

#include <memory>
#include <string>
#include <vector>

namespace strata::test_util
{

// Parses source (parse only, no semantic analysis).
inline std::unique_ptr<Module> ParseModule(std::string_view src, DiagnosticEngine& diag, std::string name = "test")
{
    Lexer lex(src, diag);
    Parser parser(lex, diag, std::move(name));
    return parser.ParseModule();
}

// Parses source and runs semantic analysis (overload resolution, type checks).
inline std::unique_ptr<Module> ParseAndResolve(std::string_view src, DiagnosticEngine& diag,
                                               std::string name = "test")
{
    auto mod = ParseModule(src, diag, std::move(name));
    if (mod)
    {
        ResolveOverloads(*mod, diag);
    }
    return mod;
}

// Lexes to EOF, returning the full token sequence.
inline std::vector<Token> LexAll(std::string_view src)
{
    DiagnosticEngine diag;
    Lexer lex(src, diag);
    std::vector<Token> toks;
    while (true)
    {
        Token t = lex.NextToken();
        toks.push_back(t);
        if (t.Is(TokKind::Eof)) break;
    }
    return toks;
}

inline std::vector<TokKind> Kinds(const std::vector<Token>& toks)
{
    std::vector<TokKind> k;
    k.reserve(toks.size());
    for (const auto& t : toks) k.push_back(t.kind);
    return k;
}

inline std::string_view TextOf(std::string_view src, const Token& t)
{
    return src.substr(t.range.start, t.range.length);
}

} // namespace strata::test_util
