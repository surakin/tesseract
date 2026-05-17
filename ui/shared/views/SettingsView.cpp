#include "SettingsView.h"

#include "tk/theme.h"

#include <algorithm>
#include <memory>

namespace tesseract::views
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SettingsView::SettingsView()
{
    // Back button — placed left-aligned inside the bar by arrange().
    auto back = std::make_unique<tk::Button>(
        "← Back",
        std::function<void()>{},
        tk::Button::Variant::Subtle);
    back->set_on_click([this] { if (on_close) { on_close(); } });
    back_btn_ = add_child(std::move(back));

    // Account section.
    auto account = std::make_unique<AccountSection>();
    account_ = account.get();

    // Appearance section.
    auto appearance = std::make_unique<AppearanceSection>();
    appearance->on_theme_changed =
        [this](tesseract::Settings::ThemePreference pref)
        {
            if (on_theme_changed) { on_theme_changed(pref); }
        };
    appearance_ = appearance.get();

    // Notifications section.
    auto notifications = std::make_unique<NotificationsSection>();
    notifications->on_notifications_changed =
        [this](bool enabled)
        {
            if (on_notifications_changed) { on_notifications_changed(enabled); }
        };
    notifications->on_image_previews_changed =
        [this](bool enabled)
        {
            if (on_image_previews_changed) { on_image_previews_changed(enabled); }
        };
    notifications_ = notifications.get();

    // SideTabView — owns the three section widgets.
    auto tabs = std::make_unique<tk::SideTabView>();
    tabs->add_tab("Account",       std::move(account));
    tabs->add_tab("Appearance",    std::move(appearance));
    tabs->add_tab("Notifications", std::move(notifications));
    // First tab is auto-selected by SideTabView::add_tab.
    tabs_ = add_child(std::move(tabs));
}

// ---------------------------------------------------------------------------
// Public setters — forwarded to child sections
// ---------------------------------------------------------------------------

void SettingsView::set_account_info(std::string display_name,
                                    std::string user_id,
                                    std::string avatar_mxc)
{
    if (!account_) { return; }
    account_->set_display_name(std::move(display_name));
    account_->set_user_id     (std::move(user_id));
    account_->set_avatar_url  (std::move(avatar_mxc));
}

void SettingsView::set_image_provider(AccountSection::ImageProvider provider)
{
    if (account_) { account_->set_image_provider(std::move(provider)); }
}

void SettingsView::set_theme_pref(tesseract::Settings::ThemePreference pref)
{
    if (appearance_) { appearance_->set_selected(pref); }
}

void SettingsView::set_notifications_enabled(bool enabled)
{
    if (notifications_) { notifications_->set_checked(enabled); }
}

void SettingsView::set_image_previews_enabled(bool enabled)
{
    if (notifications_) { notifications_->set_image_previews_checked(enabled); }
}

// ---------------------------------------------------------------------------
// tk::Widget overrides
// ---------------------------------------------------------------------------

tk::Size SettingsView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever the host gives us.
    return constraints;
}

void SettingsView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Top bar: full width, fixed height.
    const tk::Rect bar_rect = { bounds.x, bounds.y, bounds.w, kBarHeight };

    // Place the back button left-aligned with a small horizontal margin.
    // Button measures its own natural size; we let it determine its width but
    // fix the height to the bar and add a 12 px left inset.
    constexpr float kBtnHInset = 12.0f;
    if (back_btn_)
    {
        tk::Size btn_size = back_btn_->measure(ctx, { bounds.w, kBarHeight });
        const float btn_w = btn_size.w;
        const float btn_h = std::min(btn_size.h, kBarHeight);
        const float btn_y = bar_rect.y + (kBarHeight - btn_h) * 0.5f;
        back_btn_->arrange(ctx,
            { bar_rect.x + kBtnHInset, btn_y, btn_w, btn_h });
    }

    // SideTabView: remaining height below the bar + 1 px separator.
    const float tabs_y = bounds.y + kBarHeight + 1.0f;
    const float tabs_h = std::max(0.0f, bounds.h - kBarHeight - 1.0f);
    if (tabs_)
    {
        tabs_->arrange(ctx, { bounds.x, tabs_y, bounds.w, tabs_h });
    }
}

void SettingsView::paint(tk::PaintCtx& ctx)
{
    const tk::Palette& pal = ctx.theme.palette;

    // Overall background.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Back bar background (sidebar tone so it feels like chrome).
    const tk::Rect bar_rect = { bounds_.x, bounds_.y, bounds_.w, kBarHeight };
    ctx.canvas.fill_rect(bar_rect, pal.sidebar_bg);

    // 1 px separator between the back bar and the tab view.
    const tk::Rect sep_rect = {
        bounds_.x,
        bounds_.y + kBarHeight,
        bounds_.w,
        1.0f
    };
    ctx.canvas.fill_rect(sep_rect, pal.separator);

    // Paint children (back button + SideTabView).
    if (back_btn_ && back_btn_->visible()) { back_btn_->paint(ctx); }
    if (tabs_     && tabs_->visible())     { tabs_->paint(ctx);     }
}

} // namespace tesseract::views
