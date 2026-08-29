#include "AdvancedSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"

#include <memory>

namespace tesseract::views
{

AdvancedSection::AdvancedSection()
{
    const auto& s = tesseract::Settings::instance();

    auto* group = add_group(tk::tr("Advanced"));

    auto cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Use historical MSC2545 compatibility"), s.msc2545_legacy_compat);
    legacy_compat_cb_ = group->add_widget(std::move(cb));
    legacy_compat_cb_->on_change = [this](bool v)
    {
        if (on_msc2545_legacy_compat_changed) on_msc2545_legacy_compat_changed(v);
    };
    legacy_compat_cb_->on_hover_enter = [this]
    {
        if (host_)
            host_->show_tooltip(
                legacy_compat_cb_,
                tk::tr("When enabled, reads unstable-named MSC2545 fields from account "
                       "data in addition to the stable names, and enables your personal "
                       "image pack. Disable for strict stable-name-only behavior."),
                legacy_compat_cb_->bounds());
    };
    legacy_compat_cb_->on_hover_leave = [this]
    {
        if (host_) host_->hide_tooltip(legacy_compat_cb_);
    };

    auto* dev_group = add_group(tk::tr("Developer"));

    auto dev_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Enable developer mode"), s.developer_mode);
    developer_mode_cb_ = dev_group->add_widget(std::move(dev_cb));
    developer_mode_cb_->on_change = [this](bool v)
    {
        if (on_developer_mode_changed) on_developer_mode_changed(v);
    };

#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    auto* diagnostics_group = add_group(tk::tr("Diagnostics"));

    auto crash_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Save a local crash report if the app crashes"),
        s.crash_reporting_enabled);
    crash_reporting_cb_ = diagnostics_group->add_widget(std::move(crash_cb));
    crash_reporting_cb_->on_change = [this](bool v)
    {
        if (on_crash_reporting_changed) on_crash_reporting_changed(v);
    };
    crash_reporting_cb_->on_hover_enter = [this]
    {
        if (host_)
            host_->show_tooltip(
                crash_reporting_cb_,
                tk::tr("When enabled, a plain-text stack trace is written to a file "
                       "on this device if the app crashes. Nothing is sent anywhere "
                       "automatically — you can find and share the file yourself if "
                       "you want to report a bug."),
                crash_reporting_cb_->bounds());
    };
    crash_reporting_cb_->on_hover_leave = [this]
    {
        if (host_) host_->hide_tooltip(crash_reporting_cb_);
    };
#endif
}

void AdvancedSection::paint_before_children(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
}

void AdvancedSection::set_msc2545_legacy_compat(bool enabled)
{
    legacy_compat_cb_->set_checked(enabled);
}

void AdvancedSection::set_developer_mode(bool enabled)
{
    developer_mode_cb_->set_checked(enabled);
}

#ifdef TESSERACT_CRASH_HANDLER_ENABLED
void AdvancedSection::set_crash_reporting_enabled(bool enabled)
{
    crash_reporting_cb_->set_checked(enabled);
}
#endif

} // namespace tesseract::views
