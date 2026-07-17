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
}

void AdvancedSection::paint(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
    SettingsPage::paint(ctx);
}

void AdvancedSection::set_msc2545_legacy_compat(bool enabled)
{
    legacy_compat_cb_->set_checked(enabled);
}

void AdvancedSection::set_developer_mode(bool enabled)
{
    developer_mode_cb_->set_checked(enabled);
}

} // namespace tesseract::views
