#include "RoomMediaSection.h"

#include "SettingsGroup.h"

#include "tk/i18n.h"

#include <memory>
#include <string>
#include <utility>

namespace tesseract::views
{

namespace
{
using Mode = tesseract::MediaPreviewConfig::Mode;

const char* to_value(bool has_override, Mode m)
{
    if (!has_override) return "global";
    switch (m)
    {
    case Mode::Off:     return "off";
    // Private has no dedicated option in this per-room combo (see the
    // header comment) — display it as "Always" rather than leaving the
    // combo showing nothing selected, in the unlikely event a room already
    // has a stored override predating this restriction.
    case Mode::Private:
    case Mode::On:
    default:            return "on";
    }
}

std::optional<Mode> value_to_override(const std::string& v)
{
    if (v == "global") return std::nullopt;
    if (v == "off")    return Mode::Off;
    return Mode::On;
}
} // namespace

RoomMediaSection::RoomMediaSection()
{
    auto* group = add_group(tk::tr("Media previews"));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Use global default"), "global"},
        {tk::tr("Always"),             "on"},
        {tk::tr("Never"),              "off"},
    });
    combo->set_selected_value("global");
    override_combo_ = group->add_widget(std::move(combo));
    override_combo_->on_changed = [this](std::string value)
    {
        if (on_override_changed)
            on_override_changed(value_to_override(value));
    };
}

void RoomMediaSection::set_override(bool has_override, tesseract::MediaPreviewConfig::Mode mode)
{
    if (override_combo_)
        override_combo_->set_selected_value(to_value(has_override, mode));
}

void RoomMediaSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    SettingsPage::arrange(ctx, bounds);
    if (override_combo_)
        override_combo_->set_popup_clip(bounds);
}

} // namespace tesseract::views
