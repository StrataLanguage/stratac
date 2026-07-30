#include "strata/Core/SourceLocation.h"

#include <algorithm>

namespace strata
{

void SourceManager::ComputeLineStarts()
{
    m_lineStarts.clear();
    m_lineStarts.push_back(0);

    for (std::size_t i = 0; i < m_text.size(); ++i)
    {
        char c = m_text[i];

        if (c == '\n')
        {
            m_lineStarts.push_back(static_cast<std::uint32_t>(i + 1));
        }
        else if (c == '\r')
        {
            // Collapse CRLF; a lone CR also counts as a line break.
            if (i + 1 < m_text.size() && m_text[i + 1] == '\n')
            {
                continue; // handled by the '\n' on the next iteration
            }

            m_lineStarts.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

std::string_view SourceManager::Slice(SourceRange r) const noexcept
{
    if (r.start >= m_text.size())
    {
        return {};
    }

    auto end = std::min<std::size_t>(r.End(), m_text.size());

    return std::string_view(m_text.data() + r.start, end - r.start);
}

LineCol SourceManager::LineCol(std::uint32_t offset) const noexcept
{
    // Binary search for the last lineStart <= offset.
    auto it = std::ranges::upper_bound(m_lineStarts, offset);
    std::uint32_t lineIdx = static_cast<std::uint32_t>((it - m_lineStarts.begin()) - 1);

    return {.line = lineIdx + 1, .column = offset - m_lineStarts[lineIdx] + 1};
}

std::string_view SourceManager::LineText(std::uint32_t line) const noexcept
{
    if (line == 0 || line > m_lineStarts.size())
    {
        return {};
    }

    std::size_t start = m_lineStarts[line - 1];
    std::size_t end = line < m_lineStarts.size() ? m_lineStarts[line] : m_text.size();

    // Trim a trailing newline/CR so the caret lines up.
    while (end > start && (m_text[end - 1] == '\n' || m_text[end - 1] == '\r'))
    {
        --end;
    }

    return std::string_view(m_text.data() + start, end - start);
}

} // namespace strata
