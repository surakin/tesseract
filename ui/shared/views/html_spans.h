#pragma once
#include "tk/canvas.h"
#include <string_view>
#include <vector>

namespace tesseract::views {

// Parse the Matrix HTML formatted_body subset into a flat list of TextSpans.
//
// Recognised inline tags: <b>/<strong>, <i>/<em>, <code>, <del>/<s>/<strike>,
//   <a> (text kept, href ignored), <u> (no underline yet, text kept).
// Block tags: <p> (paragraph break as '\n'), <br> (line break as '\n'),
//   <pre> (treated as code block; inner <code> is redundant but harmless).
// Unknown tags are stripped; their text content is preserved.
// HTML entities decoded: &amp; &lt; &gt; &quot; &apos; &#N; &#xN;
//
// Returns a single plain-text span when `html` is empty.
std::vector<tk::TextSpan> html_to_spans(std::string_view html);

} // namespace tesseract::views
