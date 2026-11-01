#include "emoji_support.h"

#include "canvas.h"

namespace tk
{

namespace emoji = tesseract::emoji;

std::uint16_t detect_emoji_version(CanvasFactory& f)
{
    for (std::uint16_t version : emoji::emoji_versions())
    {
        bool all = true;
        for (std::string_view glyph : emoji::glyphs_introduced_in(version))
        {
            if (!f.can_render_emoji(glyph))
            {
                all = false;
                break;
            }
        }
        if (all)
        {
            return version;
        }
    }
    // Not even the oldest version draws cleanly (no colour-emoji font, or a
    // fallback that splits flags/keycaps): this font is outside what the
    // check can judge, and hiding every emoji would be worse than the
    // unfiltered picker — so don't filter.
    return kAllEmojiVersions;
}

std::uint16_t cached_emoji_version(const std::function<std::unique_ptr<CanvasFactory>()>& make)
{
    static std::optional<std::uint16_t> cached;
    if (!cached)
    {
        auto f = make();
        cached = f ? detect_emoji_version(*f) : kAllEmojiVersions;
    }
    return *cached;
}

std::optional<std::string_view> offered_emoji(std::string_view glyph, emoji::SkinTone tone,
                                              std::uint16_t max_version)
{
    const auto within = [max_version](std::string_view g)
    {
        return emoji::emoji_version(g) <= max_version;
    };
    const std::string_view toned = emoji::with_skin_tone(glyph, tone);
    if (within(toned))
    {
        return toned;
    }
    const std::string_view base = emoji::base_glyph(glyph);
    if (within(base))
    {
        return base;
    }
    return std::nullopt;
}

bool all_skin_tones_supported(std::string_view glyph, std::uint16_t max_version)
{
    for (emoji::SkinTone t : emoji::kSkinTones)
    {
        if (emoji::emoji_version(emoji::with_skin_tone(glyph, t)) > max_version)
        {
            return false;
        }
    }
    return true;
}

} // namespace tk
