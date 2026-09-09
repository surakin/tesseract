#pragma once

// Emoji classification + text/emoji run segmentation.
//
// These are pure text utilities (UTF-8 decode + a codepoint-range table) with
// no dependency on the HTML/Markdown parser they used to live beside
// (views/html_spans.cpp). They sit in tk/ so the canvas backends and the
// native-text hosts can size inline emoji without a tk -> views include.
//
// tesseract::views re-exports these names (see views/html_spans.h) so the
// view-layer callers (MessageListView, RoomListView) need no change.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tk
{

struct TextSpan; // ui/shared/tk/canvas.h

// Returns true when every non-whitespace character in `utf8` is a Unicode
// emoji codepoint (ZWJ sequences, skin-tone modifiers, variation selectors,
// regional indicators, keycap sequences). Used to pick the 2x BigEmoji font
// for emoji-only message bodies.
bool is_emoji_only(const std::string& utf8);

// Split one TextSpan into sub-spans at emoji/text boundaries so emoji grapheme
// clusters can be rendered at a larger inline-emoji size. `code`/`code_block`
// spans are returned unsplit. All formatting (bold, colour, url, ...) is
// inherited by each sub-span; only `is_emoji_run` differs between them.
std::vector<TextSpan> segment_emoji_runs(const TextSpan& src);

// Expand a span vector in-place, splitting each span at emoji/text boundaries.
void apply_emoji_segmentation(std::vector<TextSpan>& spans);

// A UTF-8 byte range classified as an emoji run by segment_emoji_runs.
struct EmojiByteRange
{
    std::size_t start_byte;
    std::size_t end_byte;
};

// Byte-range variant of segment_emoji_runs for plain-string callers (a native
// composer's current text). Returns the [start_byte, end_byte) ranges
// classified as emoji runs, ascending, half-open.
std::vector<EmojiByteRange> find_emoji_byte_ranges(const std::string& utf8);

// Cheap pre-gate: true iff `utf8` actually contains at least one emoji run.
// A byte scan (>= 0xE2 — where every renderable emoji's UTF-8 lead byte
// lives) short-circuits pure-ASCII text; only then is find_emoji_byte_ranges
// consulted for confirmation. Used by CanvasFactory::build_text so a plain
// label never allocates a segmentation vector.
bool text_has_emoji(std::string_view utf8);

} // namespace tk
