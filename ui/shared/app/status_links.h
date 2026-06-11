#pragma once
#include <string>
#include <vector>

namespace tesseract
{

// One run of a status-bar message: plain text when `url` is empty, a
// clickable hyperlink (with `text` as the visible label) otherwise.
struct StatusSegment
{
    std::string text;
    std::string url; // empty → plain text; otherwise http(s) only
};

// Parse markdown-style "[label](url)" spans out of a status message.
//
// A span becomes a link only when: the label and url are both non-empty,
// the url contains no whitespace or control characters, and the scheme is
// http:// or https:// (case-insensitive). Anything else — wrong scheme,
// empty parts, unbalanced brackets — is kept as literal text, so
// server-controlled strings can never inject javascript:/file: links.
//
// The url ends at the first ')'; URLs containing a literal ')' are not
// supported. No nesting. Plain input returns exactly one segment
// {msg, ""}, so callers can keep their unchanged plain-text path.
std::vector<StatusSegment> parse_status_links(const std::string& msg);

// True when any segment carries a url.
bool status_has_links(const std::vector<StatusSegment>& segs);

// Concatenated visible text (labels only, markup dropped) — for tooltips
// and plain-text fallbacks.
std::string status_plain_text(const std::vector<StatusSegment>& segs);

} // namespace tesseract
