#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strata
{

struct SourceRange
{
    std::uint32_t start = 0;
    std::uint16_t length = 0;

    constexpr std::uint32_t End() const noexcept
    {
        return start + length;
    }

    constexpr bool Valid() const noexcept
    {
        return length != 0 || start != 0;
    }
};

struct LineCol
{
    std::uint32_t line = 1;   // 1-based
    std::uint32_t column = 1; // 1-based
};

class SourceManager
{
public:
    SourceManager() = default;

    // Takes ownership of the source text for this translation unit.
    void SetSource(std::string text, std::string name = "<string>")
    {
        m_text = std::move(text);
        m_name = std::move(name);

        ComputeLineStarts();
    }

    std::string_view Source() const noexcept
    {
        return m_text;
    }

    std::string_view Name() const noexcept
    {
        return m_name;
    }

    std::size_t Size() const noexcept
    {
        return m_text.size();
    }

    // Text covered by a range. Returns empty view if the range is out of bounds.
    std::string_view Slice(SourceRange r) const noexcept;

    // Converts a byte offset into 1-based (line, column).
    LineCol LineCol(std::uint32_t offset) const noexcept;

    // Returns the full text of the 1-based line number, without the newline.
    std::string_view LineText(std::uint32_t line) const noexcept;

    std::uint32_t LineCount() const noexcept
    {
        return static_cast<std::uint32_t>(m_lineStarts.size());
    }

private:
    void ComputeLineStarts();

    std::string m_text;
    std::string m_name;

    // Byte offset of the first character of each line. Line N starts at
    // lineStarts_[N-1]. Always contains at least one entry (line 1 -> 0).
    std::vector<std::uint32_t> m_lineStarts{0};
};

} // namespace strata
