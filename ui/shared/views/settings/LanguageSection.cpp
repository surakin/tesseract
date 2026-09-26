#include "LanguageSection.h"

#include "SettingsGroup.h"
#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tesseract/settings.h"

namespace tesseract::views
{

LanguageSection::LanguageSection()
{
    auto* group = add_group(tk::tr("Language"));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Auto"),    "auto"},
        {tk::tr("English"), "en"},
        {tk::tr("Spanish"), "es"},
        {tk::tr("French"),  "fr"},
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

    // HBox so the button keeps its natural width instead of stretching.
    auto row = tk::create_widget<tk::HBox>(this);
    row->add_child(tk::create_widget<tk::Button>(this,
        tk::tr("Restart now"),
        [this] { if (on_restart_requested) on_restart_requested(); }));
    row->set_visible(false);
    restart_row_ = group->add_widget(std::move(row));
}

void LanguageSection::set_restart_pending(bool pending)
{
    if (restart_row_)
        restart_row_->set_visible(pending);
}

void LanguageSection::set_selected(const std::string& lang)
{
    if (combo_)
        combo_->set_selected_value(lang);
}

} // namespace tesseract::views
