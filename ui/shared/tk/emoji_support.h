#pragma once

// Which emoji the platform's font can actually draw.
//
// Emoji fonts gain whole Emoji versions at a time, so rather than probing
// every glyph, detect_emoji_version() walks the Emoji versions newest-first
// and probes the glyphs each one introduced; the first version whose glyphs
// all render is the font's version. The picker and :shortcode: autocomplete
// then offer only glyphs at or below it (offered_emoji()), so users can't
// pick or send an emoji they would see as a box or broken-up sequence.

#include <tesseract/emoji.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace tk
{

class CanvasFactory;

/// Every version in the table is supported (no probing on this platform).
constexpr std::uint16_t kAllEmojiVersions = 0xFFFF;

/// Newest Emoji version whose introduced glyphs all pass
/// `f.can_render_emoji`, walking newest → oldest and stopping at the first
/// fully supported one (font support is cumulative in practice, so older
/// versions aren't re-checked). kAllEmojiVersions when no version passes —
/// a font the check can't judge is left unfiltered rather than emptying the
/// picker. Uncached.
std::uint16_t detect_emoji_version(CanvasFactory& f);

/// detect_emoji_version() run once per process on a factory from `make`
/// (fonts are process-wide) and cached. UI thread only.
std::uint16_t cached_emoji_version(const std::function<std::unique_ptr<CanvasFactory>()>& make);

/// The glyph to offer for `glyph` with the default `tone`, given the font's
/// `max_version`: the toned variant when supported, else the base, else
/// nullopt (hide it). Glyphs outside the table (version 0) pass unchanged.
std::optional<std::string_view>
offered_emoji(std::string_view glyph, tesseract::emoji::SkinTone tone, std::uint16_t max_version);

/// True when every tone variant of `glyph` is within `max_version`.
bool all_skin_tones_supported(std::string_view glyph, std::uint16_t max_version);

} // namespace tk
