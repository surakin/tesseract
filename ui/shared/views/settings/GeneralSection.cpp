#include "GeneralSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"

namespace tesseract::views
{

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
}

void GeneralSection::set_launch_at_login(bool enabled)
{
    launch_at_login_cb_->set_checked(enabled);
}

} // namespace tesseract::views
