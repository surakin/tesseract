#include "TimezonePicker.h"
#include "iana_timezones.h"
#include "search_match_utils.h"

#include "tk/i18n.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// The id's last '/'-separated segment, e.g. "Buenos_Aires" out of
// "America/Argentina/Buenos_Aires" — lets typing a city name (without the
// region prefix) rank that zone highly.
std::string_view last_segment(std::string_view id)
{
    const auto pos = id.find_last_of('/');
    return pos == std::string_view::npos ? id : id.substr(pos + 1);
}

} // namespace

TimezonePicker::TimezonePicker()
{
    if (!host())
        return;
    init_(kFieldH, kRowH, kWidth, kMaxRows,
         tk::tr("Timezone (e.g. Europe/London)"));
}

std::size_t TimezonePicker::entry_count_() const
{
    return std::size(kIanaTimezones);
}

// Rank: 0 = exact id match, 1 = id starts-with, 2 = last-segment ("city")
// starts-with, 3 = substring anywhere in id. Returns -1 for no match at all.
int TimezonePicker::match_rank_(std::size_t index, std::string_view query) const
{
    const std::string_view id = kIanaTimezones[index].id;
    if (query.empty())
        return 3; // everything matches the empty query, lowest priority tier
    if (iequals(id, query))
        return 0;
    if (istarts_with(id, query))
        return 1;
    if (istarts_with(last_segment(id), query))
        return 2;
    if (icontains(id, query))
        return 3;
    return -1;
}

std::string TimezonePicker::entry_key_(std::size_t index) const
{
    return std::string(kIanaTimezones[index].id);
}

std::string TimezonePicker::entry_label_(std::size_t index) const
{
    std::string label(kIanaTimezones[index].id);
    std::replace(label.begin(), label.end(), '_', ' ');
    return label;
}

std::string TimezonePicker::entry_display_(std::size_t index) const
{
    // Timezone ids don't have a separate short-code/friendly-name split like
    // languages do — the (underscore-replaced) id is already the friendly
    // form, so the field shows the same text as the dropdown row.
    return entry_label_(index);
}

} // namespace tesseract::views
