#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract
{

/// One linkable URI found inside a plain-text string.
/// `start` and `end` are byte offsets (UTF-8); `url` is the verbatim URI text,
/// identical to `text.substr(start, end - start)`.
struct UrlSpan
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::string url;
};

/// Find all http(s):// URLs and `matrix:` URIs (MSC2312) in `text`.
/// Returns byte-offset spans sorted by `start`, guaranteed non-overlapping.
/// Returns an empty vector when `text` contains no linkable URI.
std::vector<UrlSpan> find_url_spans(std::string_view text);

/// Like `find_url_spans`, but excludes spans inside a Markdown code region
/// (fenced block or inline code span) — a URL shown as code was not meant
/// as a link to preview. Used for URL-preview candidate selection, not for
/// autolinking plain-text bodies into clickable links.
std::vector<UrlSpan> find_preview_url_spans(std::string_view text);

} // namespace tesseract
