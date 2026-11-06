#include "PrivacySection.h"

#include "SettingsGroup.h"

#include "views/update_checks.h"
#include "tesseract/settings.h"
#include "tk/i18n.h"

#include <memory>

namespace tesseract::views
{

PrivacySection::PrivacySection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    const auto& s = S::instance();

    // ── Privacy ───────────────────────────────────────────────────────────────
    auto* privacy_group = add_group(tk::tr("Privacy"));

    auto* presence_cb = privacy_group->add_widget(tk::create_widget<tk::CheckButton>(
        this, tk::tr("Send and receive presence status"), s.send_presence));
    binder.bind(presence_cb, &S::send_presence, SettingKey::SendPresence);

    auto invite_cb = tk::create_widget<tk::CheckButton>(this, tk::tr("Show avatars in invites"),
                                                        s.invite_avatars);
    invite_avatars_cb_ = privacy_group->add_widget(std::move(invite_cb));
    binder.bind(invite_avatars_cb_, &S::invite_avatars, SettingKey::InviteAvatars);

    // ── Data sharing ──────────────────────────────────────────────────────────
    // Created lazily: omitted entirely when both update checks and crash
    // reporting are compiled out (or update checks are off at runtime).
    [[maybe_unused]] SettingsGroup* data_group = nullptr;

#ifdef TESSERACT_UPDATE_CHECKS
    // Omitted when checks were turned off at runtime (MSIX installs are
    // updated by the Store / App Installer).
    if (tesseract::update_checks_enabled())
    {
        data_group = add_group(tk::tr("Data sharing"));
        auto updates_cb = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Check for updates automatically"), s.check_for_updates);
        auto* check_updates_cb = data_group->add_widget(std::move(updates_cb));
        binder.bind(check_updates_cb, &S::check_for_updates, SettingKey::CheckForUpdates);
    }
#endif

#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    if (!data_group)
    {
        data_group = add_group(tk::tr("Data sharing"));
    }
    auto crash_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Save a local crash report if the app crashes"), s.crash_reporting_enabled);
    crash_reporting_cb_ = data_group->add_widget(std::move(crash_cb));
    binder.bind(crash_reporting_cb_, &S::crash_reporting_enabled, SettingKey::CrashReporting);
    crash_reporting_cb_->on_hover_enter = [this]
    {
        if (host_)
        {
            host_->show_tooltip(
                crash_reporting_cb_,
                tk::tr("When enabled, a plain-text stack trace is written to a file "
                       "on this device if the app crashes. Nothing is sent anywhere "
                       "automatically — you can find and share the file yourself if "
                       "you want to report a bug."),
                crash_reporting_cb_->bounds());
        }
    };
    crash_reporting_cb_->on_hover_leave = [this]
    {
        if (host_)
        {
            host_->hide_tooltip(crash_reporting_cb_);
        }
    };
#endif
}

void PrivacySection::paint_before_children(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
    SettingsPage::paint_before_children(ctx);
}

} // namespace tesseract::views
