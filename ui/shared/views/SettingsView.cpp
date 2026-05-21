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
    auto back = std::make_unique<tk::Button>("← Back", std::function<void()>{},
                                             tk::Button::Variant::Subtle);
    back->set_on_click(
        [this]
        {
            if (on_close)
            {
                on_close();
            }
        });
    back_btn_ = add_child(std::move(back));

    // Account section.
    auto account = std::make_unique<AccountSection>();
    account_ = account.get();

    // Appearance section.
    auto appearance = std::make_unique<AppearanceSection>();
    appearance->on_theme_changed =
        [this](tesseract::Settings::ThemePreference pref)
    {
        if (on_theme_changed)
        {
            on_theme_changed(pref);
        }
    };
    appearance_ = appearance.get();

    // Notifications section.
    auto notifications = std::make_unique<NotificationsSection>();
    notifications->on_notifications_changed = [this](bool enabled)
    {
        if (on_notifications_changed)
        {
            on_notifications_changed(enabled);
        }
    };
    notifications->on_image_previews_changed = [this](bool enabled)
    {
        if (on_image_previews_changed)
        {
            on_image_previews_changed(enabled);
        }
    };
    notifications_ = notifications.get();

    // Media section.
    auto media = std::make_unique<MediaSection>();
    media->on_prefetch_changed = [this](bool enabled)
    {
        if (on_prefetch_changed)
        {
            on_prefetch_changed(enabled);
        }
    };
    media_ = media.get();

    // Server section.
    auto server = std::make_unique<ServerSection>();
    server_section_ = server.get();

    // SideTabView — owns the five section widgets.
    auto tabs = std::make_unique<tk::SideTabView>();
    tabs->add_tab("Account", std::move(account));
    tabs->add_tab("Appearance", std::move(appearance));
    tabs->add_tab("Notifications", std::move(notifications));
    tabs->add_tab("Media", std::move(media));
    tabs->add_tab("Server", std::move(server));
    // First tab is auto-selected by SideTabView::add_tab.
    tabs_ = add_child(std::move(tabs));
}

// ---------------------------------------------------------------------------
// Public setters — forwarded to child sections
// ---------------------------------------------------------------------------

void SettingsView::set_account_info(std::string display_name,
                                    std::string user_id, std::string avatar_mxc)
{
    if (!account_)
    {
        return;
    }
    account_->set_display_name(std::move(display_name));
    account_->set_user_id(std::move(user_id));
    account_->set_avatar_url(std::move(avatar_mxc));
}

void SettingsView::set_image_provider(AccountSection::ImageProvider provider)
{
    if (account_)
    {
        account_->set_image_provider(std::move(provider));
    }
}

void SettingsView::set_theme_pref(tesseract::Settings::ThemePreference pref)
{
    if (appearance_)
    {
        appearance_->set_selected(pref);
    }
}

void SettingsView::set_notifications_enabled(bool enabled)
{
    if (notifications_)
    {
        notifications_->set_checked(enabled);
    }
}

void SettingsView::set_image_previews_enabled(bool enabled)
{
    if (notifications_)
    {
        notifications_->set_image_previews_checked(enabled);
    }
}

void SettingsView::set_prefetch_enabled(bool enabled)
{
    if (media_)
    {
        media_->set_prefetch_checked(enabled);
    }
}

void SettingsView::set_server_info(const tesseract::ServerInfo& info)
{
    if (server_section_)
    {
        server_section_->set_server_info(info);
    }
    account_->set_editable(info.can_set_displayname);
    account_->set_avatar_editable(info.can_set_avatar);
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
    const tk::Rect bar_rect = {bounds.x, bounds.y, bounds.w, kBarHeight};

    // Place the back button left-aligned with a small horizontal margin.
    // Button measures its own natural size; we let it determine its width but
    // fix the height to the bar and add a 12 px left inset.
    constexpr float kBtnHInset = 12.0f;
    if (back_btn_)
    {
        tk::Size btn_size = back_btn_->measure(ctx, {bounds.w, kBarHeight});
        const float btn_w = btn_size.w;
        const float btn_h = std::min(btn_size.h, kBarHeight);
        const float btn_y = bar_rect.y + (kBarHeight - btn_h) * 0.5f;
        back_btn_->arrange(ctx, {bar_rect.x + kBtnHInset, btn_y, btn_w, btn_h});
    }

    // SideTabView: remaining height below the bar + 1 px separator.
    const float tabs_y = bounds.y + kBarHeight + 1.0f;
    const float tabs_h = std::max(0.0f, bounds.h - kBarHeight - 1.0f);
    if (tabs_)
    {
        tabs_->arrange(ctx, {bounds.x, tabs_y, bounds.w, tabs_h});
    }
}

void SettingsView::paint(tk::PaintCtx& ctx)
{
    const tk::Palette& pal = ctx.theme.palette;

    // Overall background.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Back bar background (sidebar tone so it feels like chrome).
    const tk::Rect bar_rect = {bounds_.x, bounds_.y, bounds_.w, kBarHeight};
    ctx.canvas.fill_rect(bar_rect, pal.sidebar_bg);

    // 1 px separator between the back bar and the tab view.
    const tk::Rect sep_rect = {bounds_.x, bounds_.y + kBarHeight, bounds_.w,
                               1.0f};
    ctx.canvas.fill_rect(sep_rect, pal.separator);

    // Paint children (back button + SideTabView).
    if (back_btn_ && back_btn_->visible())
    {
        back_btn_->paint(ctx);
    }
    if (tabs_ && tabs_->visible())
    {
        tabs_->paint(ctx);
    }
}

void SettingsView::set_controller(tesseract::SettingsController* ctrl)
{
    // Wire controller result/changed callbacks → AccountSection state.
    ctrl->on_avatar_result = [this](bool ok, std::string error)
    {
        account_->set_avatar_busy(false);
        if (!ok)
            account_->set_avatar_error(std::move(error));
    };
    ctrl->on_name_result = [this](bool ok, std::string error)
    {
        account_->set_name_busy(false);
        if (!ok)
            account_->set_name_error(std::move(error));
    };
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        account_->set_avatar_url(std::move(mxc));
    };
    ctrl->on_name_changed = [this](std::string name)
    {
        account_->set_display_name(std::move(name));
    };

    // Wire AccountSection click callbacks → SettingsView output callbacks.
    account_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_requested)
            on_avatar_upload_requested();
    };
    account_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_requested)
            on_avatar_remove_requested();
    };
}

void SettingsView::set_name_busy(bool busy)        { account_->set_name_busy(busy); }
void SettingsView::set_name_error(std::string e)   { account_->set_name_error(std::move(e)); }
void SettingsView::set_avatar_busy(bool busy)      { account_->set_avatar_busy(busy); }
void SettingsView::set_avatar_error(std::string e) { account_->set_avatar_error(std::move(e)); }
void SettingsView::set_avatar_url(std::string m)   { account_->set_avatar_url(std::move(m)); }
void SettingsView::set_display_name_text(std::string n) { account_->set_display_name(std::move(n)); }

tk::Rect SettingsView::name_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->name_field_rect();
}

} // namespace tesseract::views
