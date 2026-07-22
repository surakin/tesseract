#include "LanguageSection.h"

#include "SettingsGroup.h"
#include "tk/controls.h"
#include "tk/i18n.h"
#include "tesseract/settings.h"

namespace tesseract::views
{

LanguageSection::LanguageSection()
{
    auto* group = add_group("Language");

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Auto"),    "auto"},
        {tk::tr("English"), "en"},
        {tk::tr("Spanish"), "es"},
    });
    combo->set_selected_value(tesseract::Settings::instance().language);
    combo_ = group->add_widget(std::move(combo));
    combo_->on_changed = [this](std::string value)
    {
        if (on_language_changed)
            on_language_changed(std::move(value));
    };

    auto note = tk::create_widget<tk::Label>(
        this, tk::tr("Changes take effect after restart."), tk::FontRole::Small);
    group->add_widget(std::move(note));
}

void LanguageSection::set_selected(const std::string& lang)
{
    if (combo_)
        combo_->set_selected_value(lang);
}

} // namespace tesseract::views
