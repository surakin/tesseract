#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "tk/emoji_support.h"
#include <tesseract/emoji.h>
#include <tesseract/image_pack.h>

namespace tesseract::views
{

struct ShortcodeMatch
{
    int start; ///< UTF-8 byte offset of the leading ':'.
    int end; ///< UTF-8 byte offset one past the last char (not including closing ':' for prefix matches).
    std::string prefix; ///< Characters between the colons (no ':' delimiters).
};

struct ShortcodeSuggestion
{
    std::string shortcode;              ///< Without surrounding colons, e.g. "smile".
    std::string glyph;                  ///< UTF-8 emoji character. Empty for custom emoticons.
    tesseract::ImagePackImage emoticon; ///< Valid when glyph is empty.
};

/// Stateless helper — all inputs passed per call; no members.
class ShortcodeEngine
{
public:
    /// Find an open shortcode prefix (`:abc`) under or immediately before
    /// the cursor. Returns nullopt when the cursor is not inside such a sequence.
    /// Requires at least one shortcode character after the opening ':'.
    std::optional<ShortcodeMatch> find_prefix(std::string_view text, int cursor_byte_pos) const;

    /// Find a completed shortcode (`:word:`) immediately before cursor_byte_pos
    /// where the character at match.end is a space, tab, or cursor is at EOT.
    std::optional<ShortcodeMatch> find_complete(std::string_view text, int cursor_byte_pos) const;

    /// Search Unicode emoji shortcodes and custom emoticons for `prefix`.
    /// Unicode results ranked by exact/prefix/substring match quality, then
    /// custom emoticons appended. Filtered to `any(usage & Emoticon)`.
    /// Returns at most `max_results` suggestions. Returns empty for empty prefix.
    /// Unicode glyphs that support skin tones come back in `tone`; glyphs
    /// newer than `max_version` (the platform font's Emoji version, see
    /// tk/emoji_support.h) fall back to their base or are left out.
    std::vector<ShortcodeSuggestion>
    lookup(std::string_view prefix, const std::vector<tesseract::ImagePackImage>& packs,
           int max_results = 8, tesseract::emoji::SkinTone tone = tesseract::emoji::SkinTone::None,
           std::uint16_t max_version = tk::kAllEmojiVersions) const;
};

} // namespace tesseract::views
