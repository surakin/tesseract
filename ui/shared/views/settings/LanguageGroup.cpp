#include "LanguageGroup.h"

#include "tk/i18n.h"
#include "tesseract/settings.h"

namespace tesseract::views
{

LanguageGroup::LanguageGroup(SettingBinder& binder) : SettingsGroup(tk::tr("Language"))
{
    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Auto"), "auto"},
        {tk::tr("English"), "en"},
        {tk::tr("Spanish"), "es"},
        {tk::tr("French"), "fr"},
        {tk::tr("German"), "de"},
        {tk::tr("Ukrainian"), "uk"},
        {tk::tr("Norwegian Bokmål"), "nb"},
    });
    combo_ = add_widget(std::move(combo));
    binder.bind(combo_, &tesseract::Settings::language, SettingKey::Language);

    notice_ = add_widget(tk::create_widget<RestartNotice>(this));
    notice_->on_restart_requested = [this]
    {
        if (on_restart_requested)
        {
            on_restart_requested();
        }
    };
}

void LanguageGroup::set_restart_pending(bool pending)
{
    notice_->set_pending(pending);
}

} // namespace tesseract::views
