#include "strata/Core/Diagnostics.h"

#include <cstdio>

namespace strata
{

namespace
{
const char* SeverityName(DiagSeverity s) noexcept
{
    switch (s)
    {
    case DiagSeverity::Error:
        return "error";
    case DiagSeverity::Warning:
        return "warning";
    case DiagSeverity::Note:
        return "note";
    }

    return "error";
}
} // namespace

std::string DiagnosticEngine::Format(const SourceManager& src) const
{
    std::string out;

    for (const auto& d : m_diagnostics)
    {
        char line[160];
        
        LineCol lineCol = src.LineCol(d.range.start);

        int count = std::snprintf(line, sizeof(line), "%s(%u,%u): %s: ", std::string(src.Name()).c_str(), lineCol.line,
                                  lineCol.column, SeverityName(d.severity));
        if (count > 0)
        {
            out.append(line, static_cast<std::size_t>(count));
        }

        out.append(d.message);
        out.push_back('\n');

        if (d.range.length > 0)
        {
            LineCol endOfLine = src.LineCol(d.range.End() - 1);

            if (endOfLine.line == lineCol.line)
            {
                std::string_view lt = src.LineText(lineCol.line);
                out.append(lt);
                out.push_back('\n');
                out.append(static_cast<std::size_t>(lineCol.column - 1), ' ');
                out.push_back('^');
                out.append(std::min<std::size_t>(d.range.length - 1, 60), '~');
                out.push_back('\n');
            }
        }
    }

    return out;
}

} // namespace strata
