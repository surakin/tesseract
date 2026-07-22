#include "LanguagePicker.h"
#include "bcp47_languages.h"
#include "search_match_utils.h"

#include "tk/i18n.h"

namespace tesseract::views
{

LanguagePicker::LanguagePicker()
{
    if (!host())
        return;
    init_(kFieldH, kRowH, kWidth, kMaxRows, tk::tr("Language"));
}

std::size_t LanguagePicker::entry_count_() const
{
    return std::size(kBcp47Languages);
}

// Rank: 0 = exact code, 1 = code starts-with, 2 = name starts-with,
// 3 = name substring. Returns -1 for no match at all.
int LanguagePicker::match_rank_(std::size_t index, std::string_view query) const
{
    const auto& lang = kBcp47Languages[index];
    if (query.empty())
        return 3; // everything matches the empty query, lowest priority tier
    if (iequals(lang.code, query))
        return 0;
    if (istarts_with(lang.code, query))
        return 1;
    if (istarts_with(lang.name, query))
        return 2;
    if (icontains(lang.name, query))
        return 3;
    return -1;
}

std::string LanguagePicker::entry_key_(std::size_t index) const
{
    return std::string(kBcp47Languages[index].code);
}

std::string LanguagePicker::entry_label_(std::size_t index) const
{
    const auto& lang = kBcp47Languages[index];
    return std::string(lang.name) + " (" + std::string(lang.code) + ")";
}

std::string LanguagePicker::entry_display_(std::size_t index) const
{
    return std::string(kBcp47Languages[index].name);
}

} // namespace tesseract::views
