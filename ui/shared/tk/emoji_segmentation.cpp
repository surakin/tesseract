#include "emoji_segmentation.h"

#include "canvas.h" // tk::TextSpan

namespace tk
{

bool is_emoji_only(const std::string& utf8)
{
    if (utf8.empty())
    {
        return false;
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
            cp = uint32_t(c & 0x1F) << 6 | (p[1] & 0x3F);
            p += 2;
        }
        else if ((c & 0xF0) == 0xE0 && p + 2 < end)
        {
            cp = uint32_t(c & 0x0F) << 12 | uint32_t(p[1] & 0x3F) << 6 |
                 (p[2] & 0x3F);
            p += 3;
        }
        else if ((c & 0xF8) == 0xF0 && p + 3 < end)
        {
            cp = uint32_t(c & 0x07) << 18 | uint32_t(p[1] & 0x3F) << 12 |
                 uint32_t(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
        }
        else
        {
            return false; // invalid UTF-8 → not emoji-only
        }
        cps.push_back(cp);
    }

    bool has_emoji = false;
    std::size_t i = 0;
    while (i < cps.size())
    {
        uint32_t cp = cps[i];
        // Transparent combining characters — skip without counting.
        if (cp == 0x200D ||                   // ZWJ
            (cp >= 0xFE00 && cp <= 0xFE0F) || // variation selectors
            cp == 0x20E3 ||                   // combining enclosing keycap
            (cp >= 0x1F3FB && cp <= 0x1F3FF)) // skin-tone modifiers
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
            return false; // bare digit / * / # → not emoji-only
        }
        // Emoji codepoint ranges (mirrors the Twemoji fallback table).
        if (cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
            cp == 0x2122 || cp == 0x2139 ||
            (cp >= 0x2194 && cp <= 0x2199) ||
            (cp >= 0x21A9 && cp <= 0x21AA) ||
            (cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 ||
            cp == 0x23CF || (cp >= 0x23E9 && cp <= 0x23FA) ||
            cp == 0x24C2 || (cp >= 0x25AA && cp <= 0x25FE) ||
            (cp >= 0x2600 && cp <= 0x27BF) ||
            (cp >= 0x2934 && cp <= 0x2935) ||
            (cp >= 0x2B05 && cp <= 0x2B55) || cp == 0x3030 ||
            cp == 0x303D || cp == 0x3297 || cp == 0x3299 || cp == 0x1F004 ||
            cp == 0x1F0CF || (cp >= 0x1F170 && cp <= 0x1F171) ||
            (cp >= 0x1F17E && cp <= 0x1F17F) || cp == 0x1F18E ||
            (cp >= 0x1F191 && cp <= 0x1F19A) ||
            (cp >= 0x1F1E0 && cp <= 0x1F1FF) || // regional indicators
            (cp >= 0x1F201 && cp <= 0x1F251) ||
            (cp >= 0x1F300 && cp <= 0x1F9FF) || // main emoji block
            (cp >= 0x1FA00 && cp <= 0x1FAFF))   // extended
        {
            has_emoji = true;
            ++i;
            continue;
        }
        return false; // non-emoji codepoint
    }
    return has_emoji;
}

std::vector<TextSpan> segment_emoji_runs(const TextSpan& src)
{
    if (src.code || src.code_block || src.text.empty())
        return {src};

    // Decode UTF-8 to codepoints, recording the byte offset after each.
    struct CpEntry { uint32_t cp; std::size_t byte_end; };
    std::vector<CpEntry> cps;
    cps.reserve(src.text.size());
    const auto* p    = reinterpret_cast<const unsigned char*>(src.text.data());
    const auto* end  = p + src.text.size();
    const auto* base = p;
    while (p < end)
    {
        uint32_t cp;
        const unsigned char c = *p;
        if      (c < 0x80)                              { cp = c; p += 1; }
        else if ((c & 0xE0) == 0xC0 && p+1 < end)      { cp = uint32_t(c&0x1F)<<6  |(p[1]&0x3F);                                          p += 2; }
        else if ((c & 0xF0) == 0xE0 && p+2 < end)      { cp = uint32_t(c&0x0F)<<12 |uint32_t(p[1]&0x3F)<<6 |(p[2]&0x3F);                  p += 3; }
        else if ((c & 0xF8) == 0xF0 && p+3 < end)      { cp = uint32_t(c&0x07)<<18 |uint32_t(p[1]&0x3F)<<12|uint32_t(p[2]&0x3F)<<6|(p[3]&0x3F); p += 4; }
        else                                             { cp = c; p += 1; }
        cps.push_back({cp, static_cast<std::size_t>(p - base)});
    }

    // True for codepoints that always attach to the preceding cluster.
    auto is_cluster_cont = [](uint32_t cp) -> bool {
        return cp == 0x200D ||
               (cp >= 0xFE00 && cp <= 0xFE0F) ||
               cp == 0x20E3 ||
               (cp >= 0x1F3FB && cp <= 0x1F3FF);
    };
    // Emoji base codepoints (same ranges as is_emoji_only above).
    auto is_emoji_base = [](uint32_t cp) -> bool {
        return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
               cp == 0x2122 || cp == 0x2139 ||
               (cp >= 0x2194 && cp <= 0x2199) || (cp >= 0x21A9 && cp <= 0x21AA) ||
               (cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 ||
               cp == 0x23CF || (cp >= 0x23E9 && cp <= 0x23FA) ||
               cp == 0x24C2 || (cp >= 0x25AA && cp <= 0x25FE) ||
               (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2934 && cp <= 0x2935) ||
               (cp >= 0x2B05 && cp <= 0x2B55) || cp == 0x3030 ||
               cp == 0x303D || cp == 0x3297 || cp == 0x3299 || cp == 0x1F004 ||
               cp == 0x1F0CF || (cp >= 0x1F170 && cp <= 0x1F171) ||
               (cp >= 0x1F17E && cp <= 0x1F17F) || cp == 0x1F18E ||
               (cp >= 0x1F191 && cp <= 0x1F19A) || (cp >= 0x1F1E0 && cp <= 0x1F1FF) ||
               (cp >= 0x1F201 && cp <= 0x1F251) ||
               (cp >= 0x1F300 && cp <= 0x1F9FF) ||
               (cp >= 0x1FA00 && cp <= 0x1FAFF);
    };

    // Build runs: (is_emoji, byte_end_of_last_cp_in_run).
    // Cluster-continuation codepoints extend the current run.
    // Keycap sequences ([0-9*#] + optional FE0F + 20E3) count as emoji.
    struct Run { bool emoji; std::size_t end; };
    std::vector<Run> runs;
    std::size_t i = 0;
    while (i < cps.size())
    {
        const uint32_t cp = cps[i].cp;
        if (is_cluster_cont(cp))
        {
            if (runs.empty())
                runs.push_back({false, cps[i].byte_end});
            else
                runs.back().end = cps[i].byte_end;
            ++i;
            continue;
        }
        if ((cp >= 0x30 && cp <= 0x39) || cp == 0x2A || cp == 0x23)
        {
            std::size_t j = i + 1;
            if (j < cps.size() && cps[j].cp == 0xFE0F) ++j;
            if (j < cps.size() && cps[j].cp == 0x20E3)
            {
                if (!runs.empty() && runs.back().emoji)
                    runs.back().end = cps[j].byte_end;
                else
                    runs.push_back({true, cps[j].byte_end});
                i = j + 1;
                continue;
            }
        }
        const bool emoji = is_emoji_base(cp);
        if (!runs.empty() && runs.back().emoji == emoji)
            runs.back().end = cps[i].byte_end;
        else
            runs.push_back({emoji, cps[i].byte_end});
        ++i;
    }

    if (runs.empty())
        return {src};
    // Single run — no split needed; just tag if it's all emoji.
    if (runs.size() == 1)
    {
        if (!runs[0].emoji)
            return {src};
        TextSpan r = src;
        r.is_emoji_run = true;
        return {r};
    }

    std::vector<TextSpan> result;
    result.reserve(runs.size());
    std::size_t byte_pos = 0;
    for (const auto& run : runs)
    {
        TextSpan sp  = src;
        sp.text          = src.text.substr(byte_pos, run.end - byte_pos);
        sp.is_emoji_run  = run.emoji;
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
        for (auto& sub : segment_emoji_runs(sp))
            out.push_back(std::move(sub));
    spans = std::move(out);
}

std::vector<EmojiByteRange> find_emoji_byte_ranges(const std::string& utf8)
{
    std::vector<EmojiByteRange> ranges;
    if (utf8.empty())
        return ranges;

    TextSpan whole;
    whole.text = utf8;
    std::size_t byte_pos = 0;
    for (const auto& sub : segment_emoji_runs(whole))
    {
        const std::size_t end = byte_pos + sub.text.size();
        if (sub.is_emoji_run)
            ranges.push_back({byte_pos, end});
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
        return false;
    return !find_emoji_byte_ranges(std::string(utf8)).empty();
}

} // namespace tk
