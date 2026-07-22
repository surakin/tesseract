#pragma once

// LanguagePicker — a searchable BCP-47 language-code field: a
// tk::SearchablePicker backed by kBcp47Languages (bcp47_languages.h). See
// tk::SearchablePicker's own doc comment for why the dropdown is a real
// standalone native popup surface rather than a canvas-drawn overlay.

#include "tk/searchable_picker.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace tesseract::views
{

class LanguagePicker : public tk::SearchablePicker
{
protected:
    LanguagePicker();
    TK_WIDGET_FACTORY_FRIEND(LanguagePicker)

protected:
    std::size_t entry_count_() const override;
    int match_rank_(std::size_t index, std::string_view query) const override;
    std::string entry_key_(std::size_t index) const override;
    std::string entry_label_(std::size_t index) const override;
    std::string entry_display_(std::size_t index) const override;

private:
    static constexpr float kFieldH  = 32.0f;
    static constexpr float kRowH    = 28.0f;
    static constexpr float kWidth   = 220.0f;
    static constexpr int   kMaxRows = 8;
};

} // namespace tesseract::views
