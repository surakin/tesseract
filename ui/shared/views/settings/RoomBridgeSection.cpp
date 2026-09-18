#include "RoomBridgeSection.h"

#include "SettingsGroup.h"

#include "tk/i18n.h"

#include <memory>
#include <utility>

namespace tesseract::views
{

RoomBridgeSection::RoomBridgeSection()
{
    auto* group = add_group(tk::tr("Bridge Detection"));

    auto explanation = tk::create_widget<tk::Label>(
        this,
        tk::tr("Tesseract flagged this room as bridged to another chat "
               "network because it found a bridge indicator (MSC2346) "
               "published by a bridge bot in the room. That indicator isn't "
               "always published correctly, so if this room isn't actually "
               "bridged, turn the override on below to stop treating it as "
               "one."),
        tk::FontRole::Small);
    explanation->set_wrap(true);
    group->add_widget(std::move(explanation));

    auto check = tk::create_widget<tk::CheckButton>(
        this, tk::tr("This room isn't actually bridged"));
    override_check_ = group->add_widget(std::move(check));
    override_check_->on_change = [this](bool checked)
    {
        if (on_override_changed) on_override_changed(checked);
    };
}

void RoomBridgeSection::set_override(bool not_bridged)
{
    override_check_->set_checked(not_bridged);
}

} // namespace tesseract::views
