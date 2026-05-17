#pragma once
#include <string>
#include <string_view>

namespace tesseract {

struct MarkdownResult {
    std::string body;           // original typed text (unchanged)
    std::string formatted_body; // HTML; empty when no markdown was detected
};

/// Convert markdown-flavoured plain text to HTML for Matrix `formatted_body`.
/// Returns `formatted_body == ""` when the text contains no markdown markers
/// so the caller can omit the field entirely for plain-text messages.
///
/// Lives in the client layer (no UI/tk dependencies) so the single send
/// chokepoint `tesseract::Client::send_message/send_reply/send_edit` can
/// derive `formatted_body` for every shell + secondary window uniformly.
MarkdownResult markdown_to_html(std::string_view text);

} // namespace tesseract
