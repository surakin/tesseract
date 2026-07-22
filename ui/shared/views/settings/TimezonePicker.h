#pragma once

// TimezonePicker — a searchable IANA timezone-identifier field: a
// tk::SearchablePicker backed by kIanaTimezones (iana_timezones.h). See
// tk::SearchablePicker's own doc comment for why the dropdown is a real
// standalone native popup surface rather than a canvas-drawn overlay.

#include "tk/searchable_picker.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace tesseract::views
{

class TimezonePicker : public tk::SearchablePicker
{
protected:
    TimezonePicker();
    TK_WIDGET_FACTORY_FRIEND(TimezonePicker)

protected:
    std::size_t entry_count_() const override;
    int match_rank_(std::size_t index, std::string_view query) const override;
    std::string entry_key_(std::size_t index) const override;
    std::string entry_label_(std::size_t index) const override;
    std::string entry_display_(std::size_t index) const override;

private:
    static constexpr float kFieldH  = 28.0f;
    static constexpr float kRowH    = 28.0f;
    static constexpr float kWidth   = 260.0f;
    static constexpr int   kMaxRows = 8;
};

} // namespace tesseract::views
