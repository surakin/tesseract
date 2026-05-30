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

    auto combo = std::make_unique<tk::ComboBox>();
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

    auto note = std::make_unique<tk::Label>(
        tk::tr("Changes take effect after restart."), tk::FontRole::Small);
    group->add_widget(std::move(note));
}

void LanguageSection::set_selected(const std::string& lang)
{
    if (combo_)
        combo_->set_selected_value(lang);
}

void LanguageSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    SettingsPage::arrange(ctx, bounds);
    if (combo_)
        combo_->set_popup_clip(bounds);
}

} // namespace tesseract::views
