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

    auto cb = std::make_unique<tk::CheckButton>(
        tk::tr("Use historical MSC2545 compatibility"), s.msc2545_legacy_compat);
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

} // namespace tesseract::views
