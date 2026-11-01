#include "tesseract/emoji.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace tesseract::emoji
{

namespace
{

// The embedded table: ~1900 entries, generated from the official Unicode
// emoji-test.txt (UTS #51). Regenerate with client/src/emoji_data.gen.py.
// String literals are plain "..." rather than u8"..." so they bind to
// std::string_view via the const char* constructor — the source is UTF-8
// and the project compiles with UTF-8 narrow execution charset.
const std::vector<Entry>& table()
{
    static const std::vector<Entry> data = {
#include "emoji_data.inc"
    };
    return data;
}

// Marks a literal for xgettext (i18n-extract scans this file for N_) without
// translating it; the UI runs category_name() through tk::tr() when shown.
constexpr const char* N_(const char* s)
{
    return s;
}

bool ascii_iequal(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

bool ascii_icontains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (haystack.size() < needle.size())
    {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size(); ++k)
        {
            if (!ascii_iequal(haystack[i + k], needle[k]))
            {
                ok = false;
                break;
            }
        }
        if (ok)
        {
            return true;
        }
    }
    return false;
}

} // namespace

const std::vector<Entry>& all()
{
    return table();
}

std::vector<const Entry*> filter(std::string_view query)
{
    std::vector<const Entry*> out;
    for (const auto& e : table())
    {
        if (ascii_icontains(e.name, query) || ascii_icontains(e.keywords, query))
        {
            out.push_back(&e);
        }
    }
    return out;
}

std::vector<const Entry*> by_category(Category c)
{
    std::vector<const Entry*> out;
    for (const auto& e : table())
    {
        if (e.category == c)
        {
            out.push_back(&e);
        }
    }
    return out;
}

namespace
{

struct AliasEntry
{
    std::string_view glyph;
    std::string_view alias;
};
const std::vector<AliasEntry>& alias_table()
{
    static const std::vector<AliasEntry> data = {
#include "emoji_aliases.inc"
    };
    return data;
}

} // namespace

std::vector<std::pair<const Entry*, std::string_view>> by_shortcode_prefix(std::string_view prefix)
{
    std::vector<std::pair<const Entry*, std::string_view>> out;
    std::unordered_set<const Entry*> seen;

    for (const auto& e : table())
    {
        std::string_view sc = e.shortcodes;
        while (!sc.empty())
        {
            auto sp = sc.find(' ');
            std::string_view tok = (sp == std::string_view::npos) ? sc : sc.substr(0, sp);
            if (ascii_icontains(tok, prefix))
            {
                if (seen.insert(&e).second)
                {
                    out.emplace_back(&e, tok);
                }
                break;
            }
            sc = (sp == std::string_view::npos) ? std::string_view{} : sc.substr(sp + 1);
        }
    }

    for (const auto& a : alias_table())
    {
        if (!ascii_icontains(a.alias, prefix))
        {
            continue;
        }
        for (const auto& e : table())
        {
            if (e.glyph == a.glyph)
            {
                if (seen.insert(&e).second)
                {
                    out.emplace_back(&e, a.alias);
                }
                break;
            }
        }
    }

    return out;
}

const char* category_name(Category c)
{
    switch (c)
    {
    case Category::SmileysPeople:
        return N_("Smileys & People");
    case Category::AnimalsNature:
        return N_("Animals & Nature");
    case Category::FoodDrink:
        return N_("Food & Drink");
    case Category::Activities:
        return N_("Activities");
    case Category::TravelPlaces:
        return N_("Travel & Places");
    case Category::Objects:
        return N_("Objects");
    case Category::Symbols:
        return N_("Symbols");
    case Category::Flags:
        return N_("Flags");
    }
    return "?";
}

const char* category_tab_glyph(Category c)
{
    switch (c)
    {
    case Category::SmileysPeople:
        return "\xF0\x9F\x98\x80"; // 😀
    case Category::AnimalsNature:
        return "\xF0\x9F\x90\xB6"; // 🐶
    case Category::FoodDrink:
        return "\xF0\x9F\x8D\x94"; // 🍔
    case Category::Activities:
        return "\xE2\x9A\xBD"; // ⚽
    case Category::TravelPlaces:
        return "\xE2\x9C\x88\xEF\xB8\x8F"; // ✈️
    case Category::Objects:
        return "\xF0\x9F\x92\xA1"; // 💡
    case Category::Symbols:
        return "\xE2\x9D\xA4\xEF\xB8\x8F"; // ❤️
    case Category::Flags:
        return "\xF0\x9F\x8F\xB3\xEF\xB8\x8F"; // 🏳️
    }
    return "?";
}

namespace
{

struct ToneVariant
{
    std::string_view glyph;
    std::uint16_t version;
};

struct ToneRow
{
    std::string_view base;
    ToneVariant variants[5]; // Light .. Dark (C array: brace-elided rows in the .inc)
};

// Regenerate with `emoji_data.gen.py --skin-tones`.
const std::vector<ToneRow>& tone_table()
{
    static const std::vector<ToneRow> data = {
#include "emoji_skin_tones.inc"
    };
    return data;
}

// Maps base glyphs and every toned variant to their row.
const std::unordered_map<std::string_view, const ToneRow*>& tone_index()
{
    static const auto index = []
    {
        std::unordered_map<std::string_view, const ToneRow*> m;
        for (const auto& row : tone_table())
        {
            m.emplace(row.base, &row);
            for (const auto& v : row.variants)
            {
                m.emplace(v.glyph, &row);
            }
        }
        return m;
    }();
    return index;
}

const ToneRow* find_tone_row(std::string_view glyph)
{
    const auto& index = tone_index();
    auto it = index.find(glyph);
    return it == index.end() ? nullptr : it->second;
}

constexpr const char* kSkinToneKeys[] = {
    "", "light", "medium_light", "medium", "medium_dark", "dark",
};

} // namespace

bool supports_skin_tone(std::string_view glyph)
{
    return find_tone_row(glyph) != nullptr;
}

std::string_view base_glyph(std::string_view glyph)
{
    const ToneRow* row = find_tone_row(glyph);
    return row ? row->base : glyph;
}

std::string_view with_skin_tone(std::string_view glyph, SkinTone tone)
{
    const ToneRow* row = find_tone_row(glyph);
    if (!row)
    {
        return glyph;
    }
    if (tone == SkinTone::None)
    {
        return row->base;
    }
    return row->variants[static_cast<std::size_t>(tone) - 1].glyph;
}

std::uint16_t emoji_version(std::string_view glyph)
{
    static const auto index = []
    {
        std::unordered_map<std::string_view, std::uint16_t> m;
        for (const auto& e : table())
        {
            m.emplace(e.glyph, e.version);
        }
        for (const auto& row : tone_table())
        {
            for (const auto& v : row.variants)
            {
                m.emplace(v.glyph, v.version);
            }
        }
        return m;
    }();
    auto it = index.find(glyph);
    return it == index.end() ? 0 : it->second;
}

const std::vector<std::uint16_t>& emoji_versions()
{
    static const auto versions = []
    {
        std::vector<std::uint16_t> out;
        for (const auto& e : table())
        {
            out.push_back(e.version);
        }
        for (const auto& row : tone_table())
        {
            for (const auto& v : row.variants)
            {
                out.push_back(v.version);
            }
        }
        std::sort(out.begin(), out.end(), std::greater<>());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }();
    return versions;
}

std::vector<std::string_view> glyphs_introduced_in(std::uint16_t version)
{
    std::vector<std::string_view> out;
    for (const auto& e : table())
    {
        if (e.version == version)
        {
            out.push_back(e.glyph);
        }
    }
    for (const auto& row : tone_table())
    {
        for (const auto& v : row.variants)
        {
            if (v.version == version)
            {
                out.push_back(v.glyph);
            }
        }
    }
    return out;
}

const char* skin_tone_key(SkinTone tone)
{
    return kSkinToneKeys[static_cast<std::size_t>(tone)];
}

SkinTone skin_tone_from_key(std::string_view key)
{
    for (SkinTone t : kSkinTones)
    {
        if (t != SkinTone::None && key == kSkinToneKeys[static_cast<std::size_t>(t)])
        {
            return t;
        }
    }
    return SkinTone::None;
}

} // namespace tesseract::emoji
