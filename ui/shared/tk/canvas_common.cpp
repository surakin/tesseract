#include "canvas.h"

// Backend-agnostic Canvas policy shared by all four 2D backends (Direct2D,
// CoreGraphics, QPainter, Cairo+Pango). Only pure app logic lives here — the
// FontRole→weight classification and the avatar initials policy. Native
// font construction, locale-aware uppercasing, and glyph drawing stay in each
// backend's translation unit.

#include "emoji_segmentation.h"
#include "font_config.h"

#include <cstddef>
#include <vector>

namespace tk
{

int font_role_pt(FontRole role, int base_pt)
{
    note_system_base_pt(base_pt);
    return font_role_pt_with(font_config(), role, base_pt);
}

int font_role_pt_with(const FontConfig& cfg, FontRole role, int base_pt)
{
    const int base = cfg.base_pt ? cfg.base_pt : base_pt;
    const int cat_delta = cfg.delta_pt[static_cast<std::size_t>(font_category(role))];
    // Emoji inside message text follows the Messages size as well as its own
    // delta, so emoji grow with the text around them.
    const int msg_delta = cfg.delta_pt[static_cast<std::size_t>(FontCategory::Messages)];
    const int inline_base = base + msg_delta + cat_delta;

    int offset = 0;
    switch (role)
    {
    case FontRole::Small:
        offset = -4;
        break;
    case FontRole::Caption:
        offset = -2;
        break;
    case FontRole::Body:
        offset = 0;
        break;
    case FontRole::SenderName:
        offset = -1;
        break;
    case FontRole::Timestamp:
        offset = -3;
        break;
    case FontRole::SidebarName:
        offset = 0;
        break;
    case FontRole::SidebarPreview:
        offset = -2;
        break;
    case FontRole::UnreadBadge:
        offset = -2;
        break;
    case FontRole::Title:
        offset = +2;
        break;
    case FontRole::UiSemibold:
        offset = -2;
        break;
    case FontRole::BigEmoji:
        return std::max(inline_base * 2, 6);
    // Same as Body for now — the emoji-vs-text visual size is being tuned via
    // the Linux backend comp constants (kQtEmojiStockComp / kGtkEmojiStockComp)
    // and the shared kEmojiSizeAdjust knob; keeping this at the plain body
    // size lets that be compared straight across all platforms.
    case FontRole::InlineEmoji:
        return std::max(inline_base, 6);
    case FontRole::InlineCustomEmoji:
        return std::max((inline_base + 1) * 5 / 4, 6);
    case FontRole::EmojiPickerCell:
        offset = +9;
        break;
    case FontRole::ReactionEmoji:
        offset = +2;
        break;
    case FontRole::ReactionText:
        return std::max(font_role_pt_with(cfg, FontRole::ReactionEmoji, base_pt) * 4 / 5, 6);
    }
    return std::max(base + offset + cat_delta, 6);
}

bool font_role_is_semibold(FontRole role)
{
    switch (role)
    {
    case FontRole::SenderName:
    case FontRole::SidebarName:
    case FontRole::UnreadBadge:
    case FontRole::Title:
    case FontRole::UiSemibold:
        return true;
    case FontRole::Small:
    case FontRole::Caption:
    case FontRole::Body:
    case FontRole::Timestamp:
    case FontRole::SidebarPreview:
    case FontRole::BigEmoji:
    case FontRole::InlineEmoji:
    case FontRole::InlineCustomEmoji:
    case FontRole::EmojiPickerCell:
    case FontRole::ReactionEmoji:
    case FontRole::ReactionText:
        return false;
    }
    return false;
}

namespace
{

// One decoded code point and the byte range it occupies in the source.
struct CodePoint
{
    char32_t cp;
    std::size_t begin;
    std::size_t end;
    bool valid;
};

// Decode `s` into code points. A malformed or truncated sequence becomes a
// single invalid one-byte entry so the walk still makes forward progress.
std::vector<CodePoint> decode_utf8(std::string_view s)
{
    std::vector<CodePoint> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        char32_t cp = 0;
        if (c < 0x80)
        {
            len = 1;
            cp = c;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            len = 2;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
            cp = c & 0x07;
        }
        bool valid = len != 0 && len <= s.size() - i;
        for (std::size_t k = 1; valid && k < len; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            valid = (cc & 0xC0) == 0x80;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid)
        {
            out.push_back({0xFFFD, i, i + 1, false});
            ++i;
            continue;
        }
        out.push_back({cp, i, i + len, true});
        i += len;
    }
    return out;
}

// True for the small set of whitespace code points we split words on. We
// only need ASCII whitespace plus NBSP — names are display strings, not
// arbitrary Unicode word-segmentation input, and this matches what every
// backend's native predicate flagged in practice.
bool is_word_break(char32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f' ||
           cp == 0x00A0;
}

// Code points that attach to the preceding one: combining diacritics,
// variation selectors, skin tones, tag characters (subdivision flags) and
// the enclosing keycap.
bool is_cluster_continuation(char32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0000 && cp <= 0xE007F) || cp == 0x20E3;
}

bool is_regional_indicator(char32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

// Whether `cp` may start an initial. A denylist of punctuation, brackets
// and symbols rather than an allowlist of letters, so every script's letters
// (Cyrillic, CJK, Arabic, …) qualify without a Unicode category table.
// Emoji win over the ranges below (‼ ⁉ 〰 〽 © ® sit inside them).
bool is_initial_material(char32_t cp)
{
    if (is_emoji_codepoint(cp))
    {
        return true;
    }
    if (is_cluster_continuation(cp) || cp == 0x200D || cp == 0xFEFF)
    {
        return false;
    }
    const bool skip = cp < 0x30 ||                      // controls, space, !"#$%&'()*+,-./
                      (cp >= 0x3A && cp <= 0x40) ||     // :;<=>?@
                      (cp >= 0x5B && cp <= 0x60) ||     // [\]^_`
                      (cp >= 0x7B && cp <= 0xBF) ||     // {|}~, DEL, C1, Latin-1 punctuation
                      cp == 0xD7 || cp == 0xF7 ||       // × ÷
                      (cp >= 0x2000 && cp <= 0x206F) || // General Punctuation (dashes, quotes, …)
                      (cp >= 0x2308 && cp <= 0x230B) || // ⌈⌉⌊⌋
                      (cp >= 0x27E6 && cp <= 0x27EF) || // ⟦⟧⟨⟩…
                      (cp >= 0x2983 && cp <= 0x2998) || // ⦃⦄…
                      (cp >= 0x3000 && cp <= 0x303F) || // CJK punctuation, 「」【】《》
                      (cp >= 0xFF01 && cp <= 0xFF0F) || // fullwidth ASCII punctuation
                      (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
                      (cp >= 0xFF5B && cp <= 0xFF65);
    return !skip;
}

// Index one past the grapheme starting at `i`: the base plus any
// continuations, a second regional indicator (flag pair), and ZWJ-joined
// code points (👨‍👩‍👧).
std::size_t grapheme_end(const std::vector<CodePoint>& cps, std::size_t i)
{
    const bool flag = is_regional_indicator(cps[i].cp);
    std::size_t j = i + 1;
    if (flag && j < cps.size() && is_regional_indicator(cps[j].cp))
    {
        ++j;
    }
    while (j < cps.size())
    {
        if (is_cluster_continuation(cps[j].cp))
        {
            ++j;
        }
        else if (cps[j].cp == 0x200D && j + 1 < cps.size() && !is_word_break(cps[j + 1].cp))
        {
            j += 2;
        }
        else
        {
            break;
        }
    }
    return j;
}

} // namespace

std::string initials_of(std::string_view name)
{
    const std::vector<CodePoint> cps = decode_utf8(name);
    std::string out;
    int taken = 0;            // initials captured (cap at 2)
    bool need_initial = true; // current word has no initial yet
    std::size_t i = 0;
    while (i < cps.size() && taken < 2)
    {
        if (is_word_break(cps[i].cp))
        {
            need_initial = true;
            ++i;
            continue;
        }
        if (need_initial && cps[i].valid && is_initial_material(cps[i].cp))
        {
            const std::size_t j = grapheme_end(cps, i);
            out.append(name.substr(cps[i].begin, cps[j - 1].end - cps[i].begin));
            ++taken;
            need_initial = false;
            i = j;
            continue;
        }
        ++i;
    }
    if (out.empty())
    {
        out = "?";
    }
    return out;
}

} // namespace tk
