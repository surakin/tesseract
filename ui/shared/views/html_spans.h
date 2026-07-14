#pragma once
#include "tk/canvas.h"
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

// Parse the Matrix HTML formatted_body subset into a flat list of TextSpans.
//
// Recognised inline tags: <b>/<strong>, <i>/<em>, <code>, <del>/<s>/<strike>,
//   <a href="http(s)://..."> (text kept; url field populated on the span),
//   <span data-mx-spoiler[="reason"]> (MSC2010; spoiler+spoiler_reason set),
//   <u> (no underline yet, text kept).
// Block tags: <p> (paragraph break as '\n'), <br> (line break as '\n'),
//   <pre> (code block; an inner <code class="language-X"> selects the syntax
//   highlighter — see below).
// Unknown tags are stripped; their text content is preserved.
// HTML entities decoded: &amp; &lt; &gt; &quot; &apos; &#N; &#xN;
//
// Code blocks: when a <pre>/<code> carries class="language-X" and the language
// is recognized, the block is tokenized and emitted as per-token colored spans
// (TextSpan::has_color/color set). `dark` selects the light vs dark highlight
// palette. Unknown/absent languages fall back to a single uncolored monospace
// span. Topic/non-message callers may leave `dark` at its default.
//
// Returns a single plain-text span when `html` is empty.
std::vector<tk::TextSpan> html_to_spans(std::string_view html,
                                        bool             dark = false);

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

// Parse the Matrix HTML formatted_body into a list of BodyBlocks, one per
// block-level element (paragraph, heading, list item, blockquote, table row).
// This preserves block structure for proper indentation, bullet markers, and
// visual decorations (blockquote bar, heading rule) in the renderer.
//
// Block kinds:
//   Paragraph    — default inline content between block-level elements
//   Heading      — <h1>–<h6>; level = 1–6; spans are bold
//   Blockquote   — <blockquote>; level = nesting depth (1-based)
//   UnorderedItem — <ul><li>; level = list nesting depth (1-based)
//   OrderedItem  — <ol><li>; level = list nesting depth; index = item number (1-based)
//   TableRow     — <tr>; index = 0 (body row) or 1 (header row from <thead>)
//                  cells are joined with " │ " separator spans
//
// Inline formatting within each block (bold, italic, code, links, spoilers)
// works identically to html_to_spans(). @room mention detection is applied
// per-block. Empty blocks (no spans after trimming) are dropped.
struct BodyBlock
{
    enum class Kind
    {
        Paragraph,
        Heading,
        Blockquote,
        UnorderedItem,
        OrderedItem,
        TableRow,
    };
    Kind kind  = Kind::Paragraph;
    int  level = 0;  // heading: 1–6; blockquote/list: nesting depth (1-based)
    int  index = 0;  // ordered list: item number (1-based); table row: 0=body, 1=header
    std::vector<tk::TextSpan> spans;
};

std::vector<BodyBlock> html_to_blocks(std::string_view html, bool dark = false);

// Returns true when every non-whitespace character in `utf8` is a Unicode
// emoji codepoint (including ZWJ sequences, skin-tone modifiers, variation
// selectors, regional indicators, and keycap sequences). Used to pick a
// larger, emoji-only font size for emoji-only message bodies.
bool is_emoji_only(const std::string& utf8);

// Split a single TextSpan into sub-spans at emoji/text boundaries so that
// emoji grapheme clusters can be rendered at a larger inline-emoji size.
// Code and code_block spans are returned unsplit (monospace stays body size).
// All formatting properties (bold, colour, url, etc.) are inherited by each
// sub-span; only `is_emoji_run` differs between them.
std::vector<tk::TextSpan> segment_emoji_runs(const tk::TextSpan& src);

// Expand a span vector in-place, splitting each span at emoji/text
// boundaries so backends can render emoji runs at inline-emoji size.
void apply_emoji_segmentation(std::vector<tk::TextSpan>& spans);

// A UTF-8 byte range classified as an emoji run by segment_emoji_runs.
struct EmojiByteRange
{
    std::size_t start_byte;
    std::size_t end_byte;
};

// Byte-range variant of segment_emoji_runs for callers that only have a
// plain UTF-8 string (e.g. a native composer's current text), not a
// TextSpan. Returns the [start_byte, end_byte) ranges classified as emoji
// runs, in ascending order, half-open.
std::vector<EmojiByteRange> find_emoji_byte_ranges(const std::string& utf8);

} // namespace tesseract::views
