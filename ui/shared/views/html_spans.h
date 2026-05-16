#pragma once
#include "tk/canvas.h"
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views {

// Parse the Matrix HTML formatted_body subset into a flat list of TextSpans.
//
// Recognised inline tags: <b>/<strong>, <i>/<em>, <code>, <del>/<s>/<strike>,
//   <a href="http(s)://..."> (text kept; url field populated on the span),
//   <span data-mx-spoiler[="reason"]> (MSC2010; spoiler+spoiler_reason set),
//   <u> (no underline yet, text kept).
// Block tags: <p> (paragraph break as '\n'), <br> (line break as '\n'),
//   <pre> (treated as code block; inner <code> is redundant but harmless).
// Unknown tags are stripped; their text content is preserved.
// HTML entities decoded: &amp; &lt; &gt; &quot; &apos; &#N; &#xN;
//
// Returns a single plain-text span when `html` is empty.
std::vector<tk::TextSpan> html_to_spans(std::string_view html);

// Return the first http(s) URL found in an HTML formatted_body (the href
// of the first <a> tag whose href starts with http:// or https://).
// Returns empty when none is found.
std::string first_url_from_html(std::string_view html);

// Return the first http(s) URL found in a plain-text body by scanning for
// "https://" or "http://" prefixes. Strips trailing prose punctuation.
// Returns empty when none is found.
std::string first_url_from_plain(std::string_view text);

// Convert a plain-text body into TextSpans, turning bare http(s):// URLs
// (at a word boundary) into hyperlink spans — the `url` field is set and
// the visible text is the URL itself. Surrounding text becomes plain
// spans. Returns an empty vector when `text` contains no linkable URL, so
// callers can keep the cheaper plain-text layout path in that common case.
std::vector<tk::TextSpan> autolink_plain_to_spans(std::string_view text);

} // namespace tesseract::views
