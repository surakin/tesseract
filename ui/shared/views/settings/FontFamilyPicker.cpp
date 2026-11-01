#include "FontFamilyPicker.h"
#include "search_match_utils.h"

#include "tk/host.h"
#include "tk/i18n.h"

namespace tesseract::views
{

FontFamilyPicker::FontFamilyPicker(bool monospace_only)
{
    if (!host())
    {
        return;
    }
    families_.emplace_back(); // "System default"
    for (auto& f : host()->list_font_families())
    {
        if (!monospace_only || f.monospace)
        {
            families_.push_back(std::move(f.name));
        }
    }
    init_(kFieldH, kRowH, kWidth, kMaxRows, tk::tr("Font"));
}

std::size_t FontFamilyPicker::entry_count_() const
{
    return families_.size();
}

// Rank: 0 = the default entry for an empty query, 1 = exact name, 2 = name
// starts-with, 3 = name substring; -1 = no match.
int FontFamilyPicker::match_rank_(std::size_t index, std::string_view query) const
{
    if (query.empty())
    {
        return index == 0 ? 0 : 3;
    }
    if (index == 0)
    {
        return icontains(tk::tr("System default"), query) ? 3 : -1;
    }
    const std::string& name = families_[index];
    if (iequals(name, query))
    {
        return 1;
    }
    if (istarts_with(name, query))
    {
        return 2;
    }
    return icontains(name, query) ? 3 : -1;
}

std::string FontFamilyPicker::entry_key_(std::size_t index) const
{
    return families_[index];
}

std::string FontFamilyPicker::entry_label_(std::size_t index) const
{
    return index == 0 ? tk::tr("System default") : families_[index];
}

std::string FontFamilyPicker::entry_display_(std::size_t index) const
{
    return entry_label_(index);
}

} // namespace tesseract::views
