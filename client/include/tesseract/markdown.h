#pragma once
#include <string>
#include <string_view>

namespace tesseract
{

struct MarkdownResult
{
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

/// Convert a single inline run of markdown to HTML (no block-level wrapping).
/// Always HTML-escapes `& < >`, even when no markdown markers are present — so
/// the result is safe to embed inside another element (e.g. a spoiler span),
/// unlike `markdown_to_html` which returns "" for marker-free text and wraps
/// output in block elements (`<p>`, `<blockquote>`, …).
std::string markdown_inline_to_html(std::string_view text);

} // namespace tesseract
