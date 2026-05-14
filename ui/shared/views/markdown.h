#pragma once
#include <string>
#include <string_view>

namespace tesseract::views {

struct MarkdownResult {
    std::string body;           // original typed text (unchanged)
    std::string formatted_body; // HTML; empty when no markdown was detected
};

/// Convert markdown-flavoured plain text to HTML for Matrix formatted_body.
/// Returns formatted_body="" when the text contains no markdown markers so
/// the caller can omit the field entirely for plain-text messages.
MarkdownResult markdown_to_html(std::string_view text);

} // namespace tesseract::views
