#include "tesseract/emoji.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace tesseract::emoji {

namespace {

// The embedded table: ~1900 entries, generated from the official Unicode
// emoji-test.txt (UTS #51). Regenerate with client/src/emoji_data.gen.py.
// String literals are plain "..." rather than u8"..." so they bind to
// std::string_view via the const char* constructor — the source is UTF-8
// and the project compiles with UTF-8 narrow execution charset.
const std::vector<Entry>& table() {
    static const std::vector<Entry> data = {
#include "emoji_data.inc"
    };
    return data;
}

bool ascii_iequal(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

bool ascii_icontains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (!ascii_iequal(haystack[i + k], needle[k])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace

const std::vector<Entry>& all() { return table(); }

std::vector<const Entry*> filter(std::string_view query) {
    std::vector<const Entry*> out;
    for (const auto& e : table()) {
        if (ascii_icontains(e.name, query) ||
            ascii_icontains(e.keywords, query))
            out.push_back(&e);
    }
    return out;
}

std::vector<const Entry*> by_category(Category c) {
    std::vector<const Entry*> out;
    for (const auto& e : table()) {
        if (e.category == c) out.push_back(&e);
    }
    return out;
}

namespace {

struct AliasEntry { std::string_view glyph; std::string_view alias; };
const std::vector<AliasEntry>& alias_table() {
    static const std::vector<AliasEntry> data = {
#include "emoji_aliases.inc"
    };
    return data;
}

} // namespace

std::vector<std::pair<const Entry*, std::string_view>>
by_shortcode_prefix(std::string_view prefix) {
    std::vector<std::pair<const Entry*, std::string_view>> out;
    std::unordered_set<const Entry*> seen;

    for (const auto& e : table()) {
        std::string_view sc = e.shortcodes;
        while (!sc.empty()) {
            auto sp = sc.find(' ');
            std::string_view tok = (sp == std::string_view::npos) ? sc : sc.substr(0, sp);
            if (ascii_icontains(tok, prefix)) {
                if (seen.insert(&e).second)
                    out.emplace_back(&e, tok);
                break;
            }
            sc = (sp == std::string_view::npos) ? std::string_view{} : sc.substr(sp + 1);
        }
    }

    for (const auto& a : alias_table()) {
        if (!ascii_icontains(a.alias, prefix)) continue;
        for (const auto& e : table()) {
            if (e.glyph == a.glyph) {
                if (seen.insert(&e).second)
                    out.emplace_back(&e, a.alias);
                break;
            }
        }
    }

    return out;
}

const char* category_name(Category c) {
    switch (c) {
        case Category::SmileysPeople: return "Smileys & People";
        case Category::AnimalsNature: return "Animals & Nature";
        case Category::FoodDrink:     return "Food & Drink";
        case Category::Activities:    return "Activities";
        case Category::TravelPlaces:  return "Travel & Places";
        case Category::Objects:       return "Objects";
        case Category::Symbols:       return "Symbols";
        case Category::Flags:         return "Flags";
    }
    return "?";
}

const char* category_tab_glyph(Category c) {
    switch (c) {
        case Category::SmileysPeople: return "\xF0\x9F\x98\x80";  // 😀
        case Category::AnimalsNature: return "\xF0\x9F\x90\xB6";  // 🐶
        case Category::FoodDrink:     return "\xF0\x9F\x8D\x94";  // 🍔
        case Category::Activities:    return "\xE2\x9A\xBD";       // ⚽
        case Category::TravelPlaces:  return "\xE2\x9C\x88\xEF\xB8\x8F"; // ✈️
        case Category::Objects:       return "\xF0\x9F\x92\xA1";  // 💡
        case Category::Symbols:       return "\xE2\x9D\xA4\xEF\xB8\x8F"; // ❤️
        case Category::Flags:         return "\xF0\x9F\x8F\xB3\xEF\xB8\x8F"; // 🏳️
    }
    return "?";
}

} // namespace tesseract::emoji
