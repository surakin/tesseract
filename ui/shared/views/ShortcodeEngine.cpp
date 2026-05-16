#include "views/ShortcodeEngine.h"
#include <tesseract/emoji.h>
#include <algorithm>
#include <cctype>

namespace tesseract::views {

namespace {

bool is_shortcode_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '+' || c == '-';
}

bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size(); ++k)
            if (std::tolower((unsigned char)hay[i+k]) != std::tolower((unsigned char)needle[k]))
                { ok = false; break; }
        if (ok) return true;
    }
    return false;
}

// Rank: 0 = exact, 1 = starts-with, 2 = substring
int match_rank(std::string_view sc, std::string_view prefix) {
    if (sc.size() == prefix.size()) return 0;
    if (sc.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), sc.begin(),
                   [](char a, char b){ return std::tolower((unsigned char)a)
                                              == std::tolower((unsigned char)b); }))
        return 1;
    return 2;
}

} // namespace

std::optional<ShortcodeMatch>
ShortcodeEngine::find_prefix(std::string_view text, int cursor) const {
    cursor = std::min(cursor, (int)text.size());
    // Walk backward from cursor to find the start of a `:word` pattern.
    int pos = cursor;
    while (pos > 0 && is_shortcode_char(text[pos - 1]))
        --pos;
    if (pos == 0 || text[pos - 1] != ':') return std::nullopt;
    int colon = pos - 1;
    // Require at least one shortcode character after the ':'.
    // If pos == cursor the cursor is directly after ':', with no word chars yet.
    if (pos == cursor) return std::nullopt;
    // Must not be a closed shortcode (`:word:` — closing colon within range)
    for (int i = pos; i < cursor; ++i)
        if (text[i] == ':') return std::nullopt;
    std::string prefix(text.substr(pos, cursor - pos));
    return ShortcodeMatch{ colon, cursor, std::move(prefix) };
}

std::optional<ShortcodeMatch>
ShortcodeEngine::find_complete(std::string_view text, int cursor) const {
    cursor = std::min(cursor, (int)text.size());
    // Text immediately before cursor must be: space/tab OR cursor==end-of-text.
    int scan_end = cursor;
    if (scan_end > 0) {
        char delim = text[scan_end - 1];
        if (delim == ' ' || delim == '\t')
            --scan_end;
        else if (scan_end < (int)text.size())
            return std::nullopt;
    }
    // scan_end now points just after what should be the closing ':'
    if (scan_end <= 0 || text[scan_end - 1] != ':') return std::nullopt;
    int close_colon = scan_end - 1;
    // Walk backward from close_colon to find the word
    int word_end = close_colon;
    int word_start = word_end;
    while (word_start > 0 && is_shortcode_char(text[word_start - 1]))
        --word_start;
    if (word_start == 0 || text[word_start - 1] != ':') return std::nullopt;
    if (word_start == word_end) return std::nullopt;  // empty word "::"
    int open_colon = word_start - 1;
    std::string prefix(text.substr(word_start, word_end - word_start));
    return ShortcodeMatch{ open_colon, scan_end, std::move(prefix) };
}

std::vector<ShortcodeSuggestion>
ShortcodeEngine::lookup(std::string_view prefix,
                        const std::vector<tesseract::ImagePackImage>& packs,
                        int max_results) const {
    if (prefix.empty()) return {};

    using Ranked = std::pair<int, ShortcodeSuggestion>;
    std::vector<Ranked> ranked;

    auto sc_pairs = tesseract::emoji::by_shortcode_prefix(prefix);
    for (auto [entry, matched_sc] : sc_pairs) {
        int r = match_rank(matched_sc, prefix);
        ShortcodeSuggestion s;
        s.shortcode = std::string(matched_sc);
        s.glyph     = std::string(entry->glyph);
        ranked.emplace_back(r, std::move(s));
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b){ return a.first < b.first; });

    std::vector<ShortcodeSuggestion> out;
    for (auto& [r, s] : ranked) {
        if ((int)out.size() >= max_results) break;
        out.push_back(std::move(s));
    }

    for (const auto& img : packs) {
        if ((int)out.size() >= max_results) break;
        if (!any(img.usage & tesseract::PackUsage::Emoticon)) continue;
        if (!icontains(img.shortcode, prefix)) continue;
        ShortcodeSuggestion s;
        s.shortcode = img.shortcode;
        s.emoticon  = img;
        out.push_back(std::move(s));
    }

    return out;
}

} // namespace tesseract::views
