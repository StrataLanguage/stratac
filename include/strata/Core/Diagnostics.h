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

namespace strata {

enum class DiagSeverity : std::uint8_t {
    Error,
    Warning,
    Note,
};

struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Error;
    SourceRange range{};
    std::string message;
};

class DiagnosticEngine {
public:
    void report(DiagSeverity severity, SourceRange range, std::string message) {
        if (severity == DiagSeverity::Error) {
            errorCount_++;
        }
        diagnostics_.push_back({severity, range, std::move(message)});
    }

    void error(SourceRange range, std::string message) {
        report(DiagSeverity::Error, range, std::move(message));
    }
    void warning(SourceRange range, std::string message) {
        report(DiagSeverity::Warning, range, std::move(message));
    }
    void note(SourceRange range, std::string message) {
        report(DiagSeverity::Note, range, std::move(message));
    }

    std::uint32_t errorCount() const noexcept { return errorCount_; }
    bool hasErrors() const noexcept { return errorCount_ > 0; }
    std::size_t count() const noexcept { return diagnostics_.size(); }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    void clear() noexcept {
        diagnostics_.clear();
        errorCount_ = 0;
    }

    // Renders all diagnostics to text, one per line, in the form:
    //   <file>(<line>,<col>): error: <message>
    std::string format(const SourceManager& src) const;

private:
    std::vector<Diagnostic> diagnostics_;
    std::uint32_t errorCount_ = 0;
};

} // namespace strata
