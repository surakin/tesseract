#include "GeneralSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/combobox.h"
#include "tk/i18n.h"

namespace tesseract::views
{

namespace
{
using LP = tesseract::Settings::LowPowerPreference;

const char* low_power_value(LP pref)
{
    switch (pref)
    {
    case LP::On:
        return "on";
    case LP::Off:
        return "off";
    case LP::Auto:
        break;
    }
    return "auto";
}

LP low_power_from_value(const std::string& v)
{
    if (v == "on")
        return LP::On;
    if (v == "off")
        return LP::Off;
    return LP::Auto;
}

std::string low_power_description(LP pref)
{
    switch (pref)
    {
    case LP::On:
        return tk::tr("Background prefetching, search indexing and bridge "
                      "checks are always paused. Message sync and encryption "
                      "are never affected.");
    case LP::Off:
        return tk::tr("Background work always runs, even on battery.");
    case LP::Auto:
        break;
    }
    return tk::tr("Pauses background prefetching, search indexing and bridge "
                  "checks while on battery or when the system energy saver is "
                  "on. Message sync and encryption are never affected.");
}
} // namespace

GeneralSection::GeneralSection()
{
    const auto& s = tesseract::Settings::instance();

    // ── Startup ───────────────────────────────────────────────────────────────
    auto* startup_group = add_group(tk::tr("Startup"));

    auto launch_at_login_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Launch Tesseract when you log in"), s.launch_at_login);
    launch_at_login_cb_ = startup_group->add_widget(std::move(launch_at_login_cb));
    launch_at_login_cb_->on_change = [this](bool v)
    {
        if (on_launch_at_login_changed) on_launch_at_login_changed(v);
    };

    // ── Power ─────────────────────────────────────────────────────────────────
    power_group_ = add_group(tk::tr("Power"));
    // Hidden until the shell confirms this machine has a battery.
    power_group_->set_visible(false);
    auto* power_group = power_group_;

    power_group->add_widget(
        tk::create_widget<tk::Label>(this, tk::tr("Low power mode")));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Auto"), "auto"},
        {tk::tr("On"), "on"},
        {tk::tr("Off"), "off"},
    });
    combo->set_selected_value(low_power_value(s.low_power_pref));
    low_power_combo_ = power_group->add_widget(std::move(combo));

    auto desc = tk::create_widget<tk::Label>(
        this, low_power_description(s.low_power_pref), tk::FontRole::Small);
    desc->set_wrap(true);
    low_power_desc_ = power_group->add_widget(std::move(desc));

    low_power_combo_->on_changed = [this](std::string v)
    {
        const LP pref = low_power_from_value(v);
        if (low_power_desc_)
            low_power_desc_->set_text(low_power_description(pref));
        if (on_low_power_changed)
            on_low_power_changed(pref);
    };
}

void GeneralSection::set_launch_at_login(bool enabled)
{
    launch_at_login_cb_->set_checked(enabled);
}

void GeneralSection::set_low_power(tesseract::Settings::LowPowerPreference pref)
{
    if (low_power_combo_)
        low_power_combo_->set_selected_value(low_power_value(pref));
    if (low_power_desc_)
        low_power_desc_->set_text(low_power_description(pref));
}

void GeneralSection::set_low_power_available(bool available)
{
    if (power_group_)
        power_group_->set_visible(available);
}

} // namespace tesseract::views
