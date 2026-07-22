#pragma once
#include <tesseract/types.h>

#include <string>
#include <vector>

namespace tesseract::views
{

// The primary (pre-'-') subtag of a BCP-47 tag, e.g. "es-MX" -> "es".
inline std::string bcp47_primary_subtag(const std::string& tag)
{
    const auto dash = tag.find('-');
    return dash == std::string::npos ? tag : tag.substr(0, dash);
}

// Picks the pronoun entry that best matches `locale` (an app/BCP-47 locale
// tag, e.g. "en" or "es"): exact tag match first, then a primary-subtag match
// (so locale "es" matches an entry tagged "es-MX"), then the first entry as a
// last resort. Returns null only when `entries` is empty.
inline const tesseract::PronounEntry* select_pronoun_entry_for_locale(
    const std::vector<tesseract::PronounEntry>& entries, const std::string& locale)
{
    if (entries.empty())
        return nullptr;
    for (const auto& e : entries)
        if (e.language == locale)
            return &e;
    const std::string locale_primary = bcp47_primary_subtag(locale);
    for (const auto& e : entries)
        if (bcp47_primary_subtag(e.language) == locale_primary)
            return &e;
    return &entries.front();
}

// Maps an MSC4247 `grammatical_gender` value to the English possessive
// pronoun word used in narrative UI text (e.g. "Alice withdrew her request").
// MSC4247 doesn't enumerate a complete vocabulary beyond its two example
// values ("feminine", "inanimate") — anything else (absent, unrecognized, or
// a future spec addition) safely falls back to the gender-neutral "their".
inline std::string possessive_pronoun_for_gender(const std::string& grammatical_gender)
{
    if (grammatical_gender == "feminine")
        return "her";
    if (grammatical_gender == "inanimate")
        return "its";
    return "their";
}

} // namespace tesseract::views
