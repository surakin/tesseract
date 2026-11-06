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
using CA = tesseract::Settings::CloseAction;

// The dropdown's selected values are the same spellings the setting persists,
// via tesseract::close_action_to_string / _from_string.

std::string close_action_description(CA action, bool tray_available)
{
    if (!tray_available)
    {
        return tk::tr("No system tray icon is available on this system, so "
                      "closing the window always quits Tesseract.");
    }
    switch (action)
    {
    case CA::Quit:
        return tk::tr("Closing the window quits Tesseract.");
    case CA::Minimize:
        return tk::tr("Closing the window minimizes Tesseract to the "
                      "taskbar.");
    case CA::HideToTray:
        break;
    }
    return tk::tr("Closing the window hides it to the system tray. Tesseract "
                  "keeps running - use the tray icon's Quit menu item to "
                  "exit.");
}

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
    {
        return LP::On;
    }
    if (v == "off")
    {
        return LP::Off;
    }
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

GeneralSection::GeneralSection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    const auto& s = S::instance();

    // ── Startup ───────────────────────────────────────────────────────────────
    auto* startup_group = add_group(tk::tr("Startup"));

    auto launch_at_login_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Launch Tesseract when you log in"), s.launch_at_login);
    launch_at_login_cb_ = startup_group->add_widget(std::move(launch_at_login_cb));
    binder.bind(launch_at_login_cb_, &S::launch_at_login, SettingKey::LaunchAtLogin);

    auto start_minimized_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Start minimized to system tray"), s.start_minimized);
    binder.bind(startup_group->add_widget(std::move(start_minimized_cb)), &S::start_minimized,
                SettingKey::StartMinimized);

    startup_group->add_widget(
        tk::create_widget<tk::Label>(this, tk::tr("When I close the window")));

    auto close_action_combo = tk::create_widget<tk::ComboBox>(this);
    close_action_combo_ = startup_group->add_widget(std::move(close_action_combo));
    rebuild_close_action_options_();

    auto close_action_desc = tk::create_widget<tk::Label>(
        this, close_action_description(s.close_action, tray_available_), tk::FontRole::Small);
    close_action_desc->set_wrap(true);
    close_action_desc_ = startup_group->add_widget(std::move(close_action_desc));

    binder.bind<CA>(close_action_combo_, &S::close_action, SettingKey::CloseAction,
                    {{CA::HideToTray, "tray"}, {CA::Quit, "quit"}, {CA::Minimize, "minimize"}});
    // Runs after the binder's own on_changed hookup on every user change too,
    // so the description follows the selection.
    {
        auto bound = close_action_combo_->on_changed;
        close_action_combo_->on_changed = [this, bound](std::string v)
        {
            if (bound)
            {
                bound(v);
            }
            refresh_close_action_();
        };
    }
    binder.bind_custom(
        [this]
        {
            refresh_close_action_();
        });

    // ── Composer ──────────────────────────────────────────────────────────────
    auto* composer_group = add_group(tk::tr("Composer"));

    auto emoticons_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Replace text emoticons like :-) with emoji"), s.auto_replace_emoticons);
    binder.bind(composer_group->add_widget(std::move(emoticons_cb)), &S::auto_replace_emoticons,
                SettingKey::AutoReplaceEmoticons);

    // ── Power ─────────────────────────────────────────────────────────────────
    power_group_ = add_group(tk::tr("Power"));
    // Hidden until the shell confirms this machine has a battery.
    power_group_->set_visible(false);
    auto* power_group = power_group_;

    power_group->add_widget(tk::create_widget<tk::Label>(this, tk::tr("Low power mode")));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Auto"), "auto"},
        {tk::tr("On"), "on"},
        {tk::tr("Off"), "off"},
    });
    low_power_combo_ = power_group->add_widget(std::move(combo));

    auto desc = tk::create_widget<tk::Label>(this, low_power_description(s.low_power_pref),
                                             tk::FontRole::Small);
    desc->set_wrap(true);
    low_power_desc_ = power_group->add_widget(std::move(desc));

    binder.bind<LP>(low_power_combo_, &S::low_power_pref, SettingKey::LowPower,
                    {{LP::Auto, "auto"}, {LP::On, "on"}, {LP::Off, "off"}});
    {
        auto bound = low_power_combo_->on_changed;
        low_power_combo_->on_changed = [this, bound](std::string v)
        {
            if (bound)
            {
                bound(v);
            }
            if (low_power_desc_)
            {
                low_power_desc_->set_text(low_power_description(low_power_from_value(v)));
            }
        };
    }
    binder.bind_custom(
        [this]
        {
            if (low_power_desc_)
            {
                low_power_desc_->set_text(low_power_description(
                    low_power_from_value(low_power_combo_->selected_value())));
            }
        });
}

void GeneralSection::set_launch_at_login(bool enabled)
{
    launch_at_login_cb_->set_checked(enabled);
}

void GeneralSection::rebuild_close_action_options_()
{
    if (!close_action_combo_)
    {
        return;
    }
    // HideToTray is only offered when a tray icon actually exists. Without one
    // the window would hide with no way back, so the option is dropped and the
    // shells fall back to a real quit (ITrayIcon::is_available()).
    std::vector<tk::ComboBox::Option> options{
        {tk::tr("Quit Tesseract"), "quit"},
        {tk::tr("Minimize to taskbar"), "minimize"},
    };
    if (tray_available_)
    {
        options.insert(options.begin(), {tk::tr("Hide to system tray"), "tray"});
    }
    close_action_combo_->set_options(std::move(options));
}

void GeneralSection::set_tray_available(bool available)
{
    tray_available_ = available;
    rebuild_close_action_options_();
    // The stored selection may no longer be offered; resolve it for display.
    refresh_close_action_();
}

void GeneralSection::refresh_close_action_()
{
    if (!close_action_combo_)
    {
        return;
    }
    // Move the selection onto an offered option so the combo never renders a
    // blank button. Persisting that resolution is the shell's job (it owns the
    // save); this only fixes up the display.
    if (!tray_available_ && close_action_combo_->selected_value() == "tray")
    {
        close_action_combo_->set_selected_value("quit");
    }
    if (close_action_desc_)
    {
        close_action_desc_->set_text(close_action_description(
            tesseract::close_action_from_string(close_action_combo_->selected_value()),
            tray_available_));
    }
}

void GeneralSection::set_low_power_available(bool available)
{
    if (power_group_)
    {
        power_group_->set_visible(available);
    }
}

} // namespace tesseract::views
