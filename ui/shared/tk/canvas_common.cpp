#include "canvas.h"

// Backend-agnostic Canvas policy shared by all four 2D backends (Direct2D,
// CoreGraphics, QPainter, Cairo+Pango). Only pure app logic lives here — the
// FontRole→weight classification and the avatar initials word-split. Native
// font construction, locale-aware uppercasing, and glyph drawing stay in each
// backend's translation unit.

#include <cstddef>

namespace tk
{

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
    case FontRole::Body:
    case FontRole::Timestamp:
    case FontRole::SidebarPreview:
    case FontRole::BigEmoji:
    case FontRole::EmojiPickerCell:
    case FontRole::ReactionEmoji:
        return false;
    }
    return false;
}

namespace
{

// Length in bytes of the UTF-8 sequence starting with lead byte `c`.
// Returns 1 for an invalid lead byte so a malformed string still makes
// forward progress (and the bad byte is treated as its own "character").
std::size_t utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// True for the small set of whitespace code points we split words on. We
// only need ASCII whitespace plus NBSP — names are display strings, not
// arbitrary Unicode word-segmentation input, and this matches what every
// backend's native predicate flagged in practice.
bool is_word_break(const char* p, std::size_t len)
{
    if (len == 1)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\v' || c == '\f';
    }
    // U+00A0 NO-BREAK SPACE (C2 A0). Treat as a separator too so e.g.
    // "Ada Lovelace" still yields "AL".
    if (len == 2)
    {
        return static_cast<unsigned char>(p[0]) == 0xC2 &&
               static_cast<unsigned char>(p[1]) == 0xA0;
    }
    return false;
}

} // namespace

std::string initials_of(std::string_view name)
{
    std::string out;
    int taken = 0; // grapheme starts captured (cap at 2)
    bool at_word = true;
    const char* p = name.data();
    const char* end = p + name.size();
    while (p < end && taken < 2)
    {
        std::size_t len = utf8_seq_len(static_cast<unsigned char>(*p));
        if (len > static_cast<std::size_t>(end - p))
        {
            len = static_cast<std::size_t>(end - p); // truncated tail
        }
        if (is_word_break(p, len))
        {
            at_word = true;
            p += len;
            continue;
        }
        if (at_word)
        {
            out.append(p, len);
            ++taken;
            at_word = false;
        }
        p += len;
    }
    if (out.empty())
    {
        out = "?";
    }
    return out;
}

} // namespace tk
