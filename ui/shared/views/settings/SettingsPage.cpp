#include "SettingsPage.h"

#include "SettingsGroup.h"

namespace tesseract::views
{

namespace
{

// Outer page inset + spacing between adjacent groups/widgets. Matches the
// padding the per-section code used to redeclare in its own anonymous
// namespace before this base class existed.
constexpr float kPagePadX = 24.0f;
constexpr float kPagePadY = 16.0f;
constexpr float kGroupSpacing = 20.0f;

} // namespace

SettingsPage::SettingsPage()
{
    set_padding(tk::Edges{kPagePadY, kPagePadX, kPagePadY, kPagePadX});
    set_spacing(kGroupSpacing);
}

SettingsGroup* SettingsPage::add_group(std::string header)
{
    return add_child(std::make_unique<SettingsGroup>(std::move(header)));
}

} // namespace tesseract::views
