// Strata compiler: source location and source buffer management.
//
// The SourceManager owns the source text for a single translation unit and
// answers line/column queries from byte offsets. Tokens and AST nodes carry
// byte ranges ([start, start+length)) into this buffer, which keeps the core
// data structures small and cheap to copy.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

struct SourceRange {
    std::uint32_t start = 0;
    std::uint16_t length = 0;

    constexpr std::uint32_t end() const noexcept { return start + length; }
    constexpr bool valid() const noexcept { return length != 0 || start != 0; }
};

struct LineCol {
    std::uint32_t line = 1;   // 1-based
    std::uint32_t column = 1; // 1-based
};

class SourceManager {
public:
    SourceManager() = default;

    // Takes ownership of the source text for this translation unit.
    void setSource(std::string text, std::string name = "<string>") {
        text_ = std::move(text);
        name_ = std::move(name);
        computeLineStarts();
    }

    std::string_view source() const noexcept { return text_; }
    std::string_view name() const noexcept { return name_; }
    std::size_t size() const noexcept { return text_.size(); }

    // Text covered by a range. Returns empty view if the range is out of bounds.
    std::string_view slice(SourceRange r) const noexcept;

    // Converts a byte offset into 1-based (line, column).
    LineCol lineCol(std::uint32_t offset) const noexcept;

    // Returns the full text of the 1-based line number, without the newline.
    std::string_view lineText(std::uint32_t line) const noexcept;

    std::uint32_t lineCount() const noexcept {
        return static_cast<std::uint32_t>(lineStarts_.size());
    }

private:
    void computeLineStarts();

    std::string text_;
    std::string name_;
    // Byte offset of the first character of each line. Line N starts at
    // lineStarts_[N-1]. Always contains at least one entry (line 1 -> 0).
    std::vector<std::uint32_t> lineStarts_{0};
};

} // namespace strata
