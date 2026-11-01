#pragma once

// FontFamilyPicker — a searchable installed-font-family field: a
// tk::SearchablePicker over Host::list_font_families(), with a leading
// "System default" entry (empty key). Optionally limited to fixed-pitch
// families for the monospace setting. See tk::SearchablePicker for why the
// dropdown is a standalone native popup surface.

#include "tk/font_config.h"
#include "tk/searchable_picker.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

class FontFamilyPicker : public tk::SearchablePicker
{
protected:
    explicit FontFamilyPicker(bool monospace_only);
    TK_WIDGET_FACTORY_FRIEND(FontFamilyPicker)

    std::size_t entry_count_() const override;
    int match_rank_(std::size_t index, std::string_view query) const override;
    std::string entry_key_(std::size_t index) const override;
    std::string entry_label_(std::size_t index) const override;
    std::string entry_display_(std::size_t index) const override;

private:
    static constexpr float kFieldH = 28.0f;
    static constexpr float kRowH = 26.0f;
    static constexpr float kWidth = 200.0f;
    static constexpr int kMaxRows = 10; // visible rows; the list scrolls

    // Entry 0 is "System default" (empty family); the rest follow in order.
    std::vector<std::string> families_;
};

} // namespace tesseract::views
