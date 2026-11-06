#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace tesseract::views
{

// A plain-text emoticon (":-)", "<3", ...) found right before the caret.
// [start, end) is the byte range to replace with `glyph`.
struct EmoticonMatch
{
    int start;
    int end;
    std::string_view glyph;
};

// Detect a finished plain-text emoticon just before `cursor` for the
// composer's opt-in auto-replacement (Settings::auto_replace_emoticons).
// The emoticon must be a whole whitespace-delimited token from a fixed table
// (so "a:)", "12:30" and "http://" never match), outside any Markdown code
// span or fence, and followed by a space/tab before the caret, or end of
// text. At end of text only tokens that can't still grow into something
// else fire: ":)" replaces immediately, but ":p" waits for a space since the
// user may be typing ":pizza:", and "D:" waits since it may be "D:\path".
// Glyphs newer than `max_emoji_version` (the font's support, see
// tk/emoji_support.h) are skipped.
std::optional<EmoticonMatch> find_emoticon(std::string_view text, int cursor,
                                           std::uint16_t max_emoji_version);

} // namespace tesseract::views
