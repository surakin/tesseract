#include "emoji_segmentation.h"

#include "canvas.h" // tk::TextSpan

#include <algorithm>
#include <iterator>

namespace tk
{

namespace
{

struct EmojiOnlyClassification
{
    bool all_emoji_or_whitespace;
    bool has_emoji;
};

// Shared codepoint walk behind is_emoji_only() and is_emoji_only_spans():
// classifies a single UTF-8 string as "every non-whitespace codepoint is
// emoji" plus whether it actually contained any emoji at all (a
// whitespace-only or empty string is `{true, false}` — neutral, so a caller
// stitching several strings together can skip it without failing the
// overall emoji-only check).
EmojiOnlyClassification classify_emoji_only_utf8(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {true, false};
    }

    // Decode UTF-8 to codepoints.
    std::vector<uint32_t> cps;
    cps.reserve(utf8.size());
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const auto* end = p + utf8.size();
    while (p < end)
    {
        uint32_t cp;
        unsigned char c = *p;
        if (c < 0x80)
        {
            cp = c;
            p += 1;
        }
        else if ((c & 0xE0) == 0xC0 && p + 1 < end)
        {
            cp = static_cast<uint32_t>(c & 0x1F) << 6 | (p[1] & 0x3F);
            p += 2;
        }
        else if ((c & 0xF0) == 0xE0 && p + 2 < end)
        {
            cp = static_cast<uint32_t>(c & 0x0F) << 12 | static_cast<uint32_t>(p[1] & 0x3F) << 6 |
                 (p[2] & 0x3F);
            p += 3;
        }
        else if ((c & 0xF8) == 0xF0 && p + 3 < end)
        {
            cp = static_cast<uint32_t>(c & 0x07) << 18 | static_cast<uint32_t>(p[1] & 0x3F) << 12 |
                 static_cast<uint32_t>(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
        }
        else
        {
            return {false, false}; // invalid UTF-8 → not emoji-only
        }
        cps.push_back(cp);
    }

    bool has_emoji = false;
    std::size_t i = 0;
    while (i < cps.size())
    {
        uint32_t cp = cps[i];
        // Transparent combining characters — skip without counting.
        if (cp == 0x200D ||                     // ZWJ
            (cp >= 0xFE00 && cp <= 0xFE0F) ||   // variation selectors
            cp == 0x20E3 ||                     // combining enclosing keycap
            (cp >= 0x1F3FB && cp <= 0x1F3FF) || // skin-tone modifiers
            (cp >= 0xE0000 && cp <= 0xE007F))   // tag characters (flag sequences)
        {
            ++i;
            continue;
        }
        // Whitespace.
        if (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D)
        {
            ++i;
            continue;
        }
        // Keycap sequence: [0-9*#] followed by optional U+FE0F then U+20E3.
        if ((cp >= 0x30 && cp <= 0x39) || cp == 0x2A || cp == 0x23)
        {
            std::size_t j = i + 1;
            if (j < cps.size() && cps[j] == 0xFE0F)
            {
                ++j;
            }
            if (j < cps.size() && cps[j] == 0x20E3)
            {
                has_emoji = true;
                i = j + 1;
                continue;
            }
            return {false, false}; // bare digit / * / # → not emoji-only
        }
        if (is_emoji_codepoint(cp))
        {
            has_emoji = true;
            ++i;
            continue;
        }
        return {false, false}; // non-emoji codepoint
    }
    return {true, has_emoji};
}

} // namespace

namespace
{

struct CpRange
{
    char32_t lo;
    char32_t hi;
};

// Codepoints in the BMP symbol blocks that have the Unicode Emoji property.
// The blocks themselves also hold plain text symbols (e.g. U+273F ✿) that
// must keep the surrounding text's font and size.
constexpr CpRange kBmpSymbolEmoji[] = {
    {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2604},
    {0x260E, 0x260E}, {0x2611, 0x2611}, {0x2614, 0x2615}, {0x2618, 0x2618}, {0x261D, 0x261D},
    {0x2620, 0x2620}, {0x2622, 0x2623}, {0x2626, 0x2626}, {0x262A, 0x262A}, {0x262E, 0x262F},
    {0x2638, 0x263A}, {0x2640, 0x2640}, {0x2642, 0x2642}, {0x2648, 0x2653}, {0x265F, 0x2660},
    {0x2663, 0x2663}, {0x2665, 0x2666}, {0x2668, 0x2668}, {0x267B, 0x267B}, {0x267E, 0x267F},
    {0x2692, 0x2697}, {0x2699, 0x2699}, {0x269B, 0x269C}, {0x26A0, 0x26A1}, {0x26A7, 0x26A7},
    {0x26AA, 0x26AB}, {0x26B0, 0x26B1}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26C8, 0x26C8},
    {0x26CE, 0x26CF}, {0x26D1, 0x26D1}, {0x26D3, 0x26D4}, {0x26E9, 0x26EA}, {0x26F0, 0x26F5},
    {0x26F7, 0x26FA}, {0x26FD, 0x26FD}, {0x2702, 0x2702}, {0x2705, 0x2705}, {0x2708, 0x270D},
    {0x270F, 0x270F}, {0x2712, 0x2712}, {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D},
    {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734}, {0x2744, 0x2744}, {0x2747, 0x2747},
    {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2763, 0x2764},
    {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B05, 0x2B07},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
};

} // namespace

bool is_emoji_codepoint(char32_t cp)
{
    if (cp < 0x00A9)
    {
        return false; // ASCII / Latin-1 fast path: nothing below © is emoji
    }
    // kBmpSymbolEmoji is sorted and non-overlapping: find the last range
    // starting at or before cp and check whether cp falls inside it.
    const auto* it = std::upper_bound(std::begin(kBmpSymbolEmoji), std::end(kBmpSymbolEmoji), cp,
                                      [](char32_t c, const CpRange& r)
                                      {
                                          return c < r.lo;
                                      });
    if (it != std::begin(kBmpSymbolEmoji) && cp <= std::prev(it)->hi)
    {
        return true;
    }
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 || cp == 0x2122 ||
           cp == 0x2139 || (cp >= 0x2194 && cp <= 0x2199) || (cp >= 0x21A9 && cp <= 0x21AA) ||
           (cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 || cp == 0x23CF ||
           (cp >= 0x23E9 && cp <= 0x23FA) || cp == 0x24C2 || (cp >= 0x2934 && cp <= 0x2935) ||
           cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299 || cp == 0x1F004 ||
           cp == 0x1F0CF || (cp >= 0x1F170 && cp <= 0x1F171) || (cp >= 0x1F17E && cp <= 0x1F17F) ||
           cp == 0x1F18E || (cp >= 0x1F191 && cp <= 0x1F19A) ||
           (cp >= 0x1F1E0 && cp <= 0x1F1FF) || // regional indicators
           (cp >= 0x1F201 && cp <= 0x1F251) ||
           (cp >= 0x1F300 && cp <= 0x1F9FF) || // main emoji block
           (cp >= 0x1FA00 && cp <= 0x1FAFF);   // extended
}

bool is_emoji_only(const std::string& utf8)
{
    const auto r = classify_emoji_only_utf8(utf8);
    return r.all_emoji_or_whitespace && r.has_emoji;
}

bool is_emoji_cluster(std::string_view utf8)
{
    const auto r = classify_emoji_only_utf8(utf8);
    return r.all_emoji_or_whitespace && r.has_emoji;
}

bool is_emoji_only_spans(const std::vector<TextSpan>& spans)
{
    bool has_emoji = false;
    for (const auto& sp : spans)
    {
        if (sp.is_image)
        {
            if (sp.pill_kind != PillKind::Generic)
            {
                return false; // a mention pill → not emoji-only
            }
            has_emoji = true;
            continue;
        }
        const auto r = classify_emoji_only_utf8(sp.text);
        if (!r.all_emoji_or_whitespace)
        {
            return false;
        }
        has_emoji = has_emoji || r.has_emoji;
    }
    return has_emoji;
}

std::vector<TextSpan> segment_emoji_runs(const TextSpan& src)
{
    if (src.code || src.code_block || src.text.empty())
    {
        return {src};
    }

    // Decode UTF-8 to codepoints, recording the byte offset after each.
    struct CpEntry
    {
        uint32_t cp;
        std::size_t byte_end;
    };
    std::vector<CpEntry> cps;
    cps.reserve(src.text.size());
    const auto* p = reinterpret_cast<const unsigned char*>(src.text.data());
    const auto* end = p + src.text.size();
    const auto* base = p;
    while (p < end)
    {
        uint32_t cp;
        const unsigned char c = *p;
        if (c < 0x80)
        {
            cp = c;
            p += 1;
        }
        else if ((c & 0xE0) == 0xC0 && p + 1 < end)
        {
            cp = static_cast<uint32_t>(c & 0x1F) << 6 | (p[1] & 0x3F);
            p += 2;
        }
        else if ((c & 0xF0) == 0xE0 && p + 2 < end)
        {
            cp = static_cast<uint32_t>(c & 0x0F) << 12 | static_cast<uint32_t>(p[1] & 0x3F) << 6 |
                 (p[2] & 0x3F);
            p += 3;
        }
        else if ((c & 0xF8) == 0xF0 && p + 3 < end)
        {
            cp = static_cast<uint32_t>(c & 0x07) << 18 | static_cast<uint32_t>(p[1] & 0x3F) << 12 |
                 static_cast<uint32_t>(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
        }
        else
        {
            cp = c;
            p += 1;
        }
        cps.push_back({cp, static_cast<std::size_t>(p - base)});
    }

    // True for codepoints that always attach to the preceding cluster.
    auto is_cluster_cont = [](uint32_t cp) -> bool
    {
        return cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x20E3 ||
               (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
               (cp >= 0xE0000 && cp <= 0xE007F); // tag characters (flag sequences)
    };

    // Build runs: (is_emoji, byte_end_of_last_cp_in_run).
    // Cluster-continuation codepoints extend the current run.
    // Keycap sequences ([0-9*#] + optional FE0F + 20E3) count as emoji.
    struct Run
    {
        bool emoji;
        std::size_t end;
    };
    std::vector<Run> runs;
    std::size_t i = 0;
    while (i < cps.size())
    {
        const uint32_t cp = cps[i].cp;
        if (is_cluster_cont(cp))
        {
            if (runs.empty())
            {
                runs.push_back({false, cps[i].byte_end});
            }
            else
            {
                runs.back().end = cps[i].byte_end;
            }
            ++i;
            continue;
        }
        if ((cp >= 0x30 && cp <= 0x39) || cp == 0x2A || cp == 0x23)
        {
            std::size_t j = i + 1;
            if (j < cps.size() && cps[j].cp == 0xFE0F)
            {
                ++j;
            }
            if (j < cps.size() && cps[j].cp == 0x20E3)
            {
                if (!runs.empty() && runs.back().emoji)
                {
                    runs.back().end = cps[j].byte_end;
                }
                else
                {
                    runs.push_back({true, cps[j].byte_end});
                }
                i = j + 1;
                continue;
            }
        }
        const bool emoji = is_emoji_codepoint(cp);
        if (!runs.empty() && runs.back().emoji == emoji)
        {
            runs.back().end = cps[i].byte_end;
        }
        else
        {
            runs.push_back({emoji, cps[i].byte_end});
        }
        ++i;
    }

    if (runs.empty())
    {
        return {src};
    }
    // Single run — no split needed; just tag if it's all emoji.
    if (runs.size() == 1)
    {
        if (!runs[0].emoji)
        {
            return {src};
        }
        TextSpan r = src;
        r.is_emoji_run = true;
        return {r};
    }

    std::vector<TextSpan> result;
    result.reserve(runs.size());
    std::size_t byte_pos = 0;
    for (const auto& run : runs)
    {
        TextSpan sp = src;
        sp.text = src.text.substr(byte_pos, run.end - byte_pos);
        sp.is_emoji_run = run.emoji;
        result.push_back(std::move(sp));
        byte_pos = run.end;
    }
    return result;
}

void apply_emoji_segmentation(std::vector<TextSpan>& spans)
{
    std::vector<TextSpan> out;
    out.reserve(spans.size());
    for (const auto& sp : spans)
    {
        for (auto& sub : segment_emoji_runs(sp))
        {
            out.push_back(std::move(sub));
        }
    }
    spans = std::move(out);
}

std::vector<EmojiByteRange> find_emoji_byte_ranges(const std::string& utf8)
{
    std::vector<EmojiByteRange> ranges;
    if (utf8.empty())
    {
        return ranges;
    }

    TextSpan whole;
    whole.text = utf8;
    std::size_t byte_pos = 0;
    for (const auto& sub : segment_emoji_runs(whole))
    {
        const std::size_t end = byte_pos + sub.text.size();
        if (sub.is_emoji_run)
        {
            ranges.push_back({byte_pos, end});
        }
        byte_pos = end;
    }
    return ranges;
}

bool text_has_emoji(std::string_view utf8)
{
    bool maybe = false;
    for (unsigned char c : utf8)
    {
        if (c >= 0xE2)
        {
            maybe = true;
            break;
        }
    }
    if (!maybe)
    {
        return false;
    }
    return !find_emoji_byte_ranges(std::string(utf8)).empty();
}

std::vector<std::size_t>
utf16_indices_for_byte_offsets(std::string_view utf8, const std::vector<std::size_t>& byte_offsets)
{
    std::vector<std::size_t> order(byte_offsets.size());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b)
              {
                  return byte_offsets[a] < byte_offsets[b];
              });

    std::vector<std::size_t> out(byte_offsets.size());
    std::size_t byte = 0;
    std::size_t units = 0;
    for (std::size_t i : order)
    {
        const std::size_t target = std::min(byte_offsets[i], utf8.size());
        while (byte < target)
        {
            // UTF-8 sequence length from the lead byte; stray continuation bytes
            // count as one unit, as QString::fromUtf8 renders them.
            const auto lead = static_cast<unsigned char>(utf8[byte]);
            std::size_t len = 1;
            if (lead >= 0xF0)
            {
                len = 4;
                units += 2; // a surrogate pair in UTF-16
            }
            else if (lead >= 0xE0)
            {
                len = 3;
                units += 1;
            }
            else if (lead >= 0xC0)
            {
                len = 2;
                units += 1;
            }
            else
            {
                units += 1;
            }
            byte += len;
        }
        out[i] = units;
    }
    return out;
}

} // namespace tk
