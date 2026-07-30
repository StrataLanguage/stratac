// Strata compiler: diagnostics.
//
// A lightweight diagnostic engine that collects messages with associated source
// ranges and reports whether any errors occurred. The parser, lexer, and later
// semantic analysis all funnel problems through here so the driver can render a
// single, consistent set of messages.
#pragma once

#include "strata/Core/SourceLocation.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace strata
{

enum class DiagSeverity : std::uint8_t
{
    Error,
    Warning,
    Note,
};

struct Diagnostic
{
    DiagSeverity severity = DiagSeverity::Error;
    SourceRange range{};
    std::string message;
};

class DiagnosticEngine
{
  public:
    void Report(DiagSeverity severity, SourceRange range, std::string message)
    {
        if (severity == DiagSeverity::Error)
        {
            m_errorCount++;
        }

        m_diagnostics.push_back({.severity = severity, .range = range, .message = std::move(message)});
    }

    void Error(SourceRange range, std::string message)
    {
        Report(DiagSeverity::Error, range, std::move(message));
    }

    void Warning(SourceRange range, std::string message)
    {
        Report(DiagSeverity::Warning, range, std::move(message));
    }

    void Note(SourceRange range, std::string message)
    {
        Report(DiagSeverity::Note, range, std::move(message));
    }

    std::uint32_t ErrorCount() const noexcept
    {
        return m_errorCount;
    }

    bool HasErrors() const noexcept
    {
        return m_errorCount > 0;
    }

    std::size_t Count() const noexcept
    {
        return m_diagnostics.size();
    }

    const std::vector<Diagnostic>& Diagnostics() const noexcept
    {
        return m_diagnostics;
    }

    void Clear() noexcept
    {
        m_diagnostics.clear();
        m_errorCount = 0;
    }

    // Renders all diagnostics to text, one per line, in the form:
    //   <file>(<line>,<col>): error: <message>
    std::string Format(const SourceManager& src) const;

  private:
    std::vector<Diagnostic> m_diagnostics;
    std::uint32_t m_errorCount = 0;
};

} // namespace strata
