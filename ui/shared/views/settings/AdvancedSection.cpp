#include "AdvancedSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"

#include <memory>

namespace tesseract::views
{

AdvancedSection::AdvancedSection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    const auto& s = S::instance();

    auto* dev_group = add_group(tk::tr("Developer"));

    auto dev_cb =
        tk::create_widget<tk::CheckButton>(this, tk::tr("Enable developer mode"), s.developer_mode);
    developer_mode_cb_ = dev_group->add_widget(std::move(dev_cb));

    auto unhandled_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Show unhandled events in the timeline"), s.show_unhandled_events);
    show_unhandled_events_cb_ = dev_group->add_widget(std::move(unhandled_cb));
    show_unhandled_events_cb_->set_enabled(s.developer_mode);
    binder.bind(show_unhandled_events_cb_, &S::show_unhandled_events,
                SettingKey::ShowUnhandledEvents);

    // Bound after the dependent checkbox exists: `local` greys it out live
    // when developer mode is switched off (its saved value is kept).
    binder.bind(developer_mode_cb_, &S::developer_mode, SettingKey::DeveloperMode,
                [this](bool on)
                {
                    show_unhandled_events_cb_->set_enabled(on);
                });

    dev_group->add_widget(tk::create_widget<tk::Button>(
        this, tk::tr("Open Activity Monitor"),
        [this]
        {
            if (on_open_activity_monitor)
            {
                on_open_activity_monitor();
            }
        },
        tk::Button::Variant::Subtle));
}

} // namespace tesseract::views
