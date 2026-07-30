#include "strata/Core/SourceLocation.h"

#include <algorithm>

namespace strata {

void SourceManager::computeLineStarts() {
    lineStarts_.clear();
    lineStarts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        char c = text_[i];
        if (c == '\n') {
            lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
        } else if (c == '\r') {
            // Collapse CRLF; a lone CR also counts as a line break.
            if (i + 1 < text_.size() && text_[i + 1] == '\n') {
                continue; // handled by the '\n' on the next iteration
            }
            lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

std::string_view SourceManager::slice(SourceRange r) const noexcept {
    if (r.start >= text_.size()) return {};
    auto end = std::min<std::size_t>(r.end(), text_.size());
    return std::string_view(text_.data() + r.start, end - r.start);
}

LineCol SourceManager::lineCol(std::uint32_t offset) const noexcept {
    // Binary search for the last lineStart <= offset.
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    std::uint32_t lineIdx = static_cast<std::uint32_t>((it - lineStarts_.begin()) - 1);
    return {lineIdx + 1, offset - lineStarts_[lineIdx] + 1};
}

std::string_view SourceManager::lineText(std::uint32_t line) const noexcept {
    if (line == 0 || line > lineStarts_.size()) return {};
    std::size_t start = lineStarts_[line - 1];
    std::size_t end = line < lineStarts_.size() ? lineStarts_[line] : text_.size();
    // Trim a trailing newline/CR so the caret lines up.
    while (end > start && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
        --end;
    }
    return std::string_view(text_.data() + start, end - start);
}

} // namespace strata
