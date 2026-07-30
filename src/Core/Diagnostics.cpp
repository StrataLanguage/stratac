#include "strata/Core/Diagnostics.h"

#include <cstdio>

namespace strata {

namespace {
const char* severityName(DiagSeverity s) noexcept {
    switch (s) {
        case DiagSeverity::Error:   return "error";
        case DiagSeverity::Warning: return "warning";
        case DiagSeverity::Note:    return "note";
    }
    return "error";
}
} // namespace

std::string DiagnosticEngine::format(const SourceManager& src) const {
    std::string out;
    for (const auto& d : diagnostics_) {
        char line[160];
        LineCol lc = src.lineCol(d.range.start);
        int n = std::snprintf(line, sizeof(line), "%s(%u,%u): %s: ",
                              std::string(src.name()).c_str(), lc.line, lc.column,
                              severityName(d.severity));
        if (n > 0) out.append(line, static_cast<std::size_t>(n));
        out.append(d.message);
        out.push_back('\n');

        // Caret line for extra context when the range is on a single line.
        if (d.range.length > 0) {
            LineCol endLc = src.lineCol(d.range.end() - 1);
            if (endLc.line == lc.line) {
                std::string_view lt = src.lineText(lc.line);
                out.append(lt);
                out.push_back('\n');
                out.append(static_cast<std::size_t>(lc.column - 1), ' ');
                out.push_back('^');
                out.append(std::min<std::size_t>(d.range.length - 1, 60), '~');
                out.push_back('\n');
            }
        }
    }
    return out;
}

} // namespace strata
