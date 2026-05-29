#include "SettingsWidget.h"
#include "tk/theme.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace gtk4
{

SettingsWidget::SettingsWidget()
    : surface_(std::make_unique<tk::gtk4::Surface>(tk::Theme::light()))
{
    auto view = std::make_unique<tesseract::views::SettingsView>();
    settings_view_ = view.get();

    settings_view_->on_close = [this]
    {
        if (on_close)
        {
            on_close();
        }
    };
    settings_view_->on_logout = [this]
    {
        if (on_logout)
        {
            on_logout();
        }
    };
    settings_view_->on_theme_changed = [this](auto p)
    {
        if (on_theme_changed)
        {
            on_theme_changed(p);
        }
    };
    settings_view_->on_notifications_changed = [this](bool e)
    {
        if (on_notifications_changed)
        {
            on_notifications_changed(e);
        }
    };
    settings_view_->on_send_presence_changed = [this](bool e)
    {
        if (on_send_presence_changed)
            on_send_presence_changed(e);
    };
    settings_view_->on_media_previews_changed =
        [this](tesseract::Settings::MediaPreviews mode)
    {
        if (on_media_previews_changed)
            on_media_previews_changed(mode);
    };
    settings_view_->on_invite_avatars_changed = [this](bool e)
    {
        if (on_invite_avatars_changed)
            on_invite_avatars_changed(e);
    };
    settings_view_->on_group_inactive_changed = [this](bool v)
    {
        if (on_group_inactive_changed)
            on_group_inactive_changed(v);
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed)
            on_inactive_period_changed(days);
    };
    // Persisted directly here (self-contained — no extra wrapper/MainWindow
    // plumbing); the lock-screen privacy gate is always on regardless.
    settings_view_->on_hide_content_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_hide_content = e;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_image_previews_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_image_previews = e;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_prefetch_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.prefetch_full_media = e;
        s.save_to_disk(tesseract::config_dir());
    };

    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

    settings_view_->on_clear_caches = [this]
    {
        if (on_clear_caches) on_clear_caches();
    };

    surface_->set_root(std::move(view));

    surface_->set_on_layout(
        [this]
        {
            if (name_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->name_field_rect();
                name_field_->set_visible(!r.empty());
                if (!r.empty())
                    name_field_->set_rect(r);
            }
        });
}

GtkWidget* SettingsWidget::widget() const
{
    return surface_->widget();
}

void SettingsWidget::set_server_info(const tesseract::ServerInfo& info)
{
    if (settings_view_)
        settings_view_->set_server_info(info);
}

void SettingsWidget::set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                                     uint64_t memory_bytes)
{
    if (settings_view_)
        settings_view_->set_cache_sizes(local_bytes, sdk_bytes, memory_bytes);
}

void SettingsWidget::set_theme(const tk::Theme& t)
{
    surface_->set_theme(t);
    surface_->relayout();
}

void SettingsWidget::populate(
    std::string display_name, std::string user_id, std::string avatar_mxc,
    tesseract::views::AccountSection::ImageProvider provider,
    tesseract::Settings::ThemePreference theme_pref, bool notifications_enabled)
{
    settings_view_->set_account_info(std::move(display_name),
                                     std::move(user_id), std::move(avatar_mxc));
    settings_view_->set_image_provider(std::move(provider));
    settings_view_->set_theme_pref(theme_pref);
    settings_view_->set_notifications_enabled(notifications_enabled);
    settings_view_->set_hide_content_enabled(
        tesseract::Settings::instance().notification_hide_content);
    settings_view_->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    settings_view_->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    settings_view_->set_send_presence_pref(
        tesseract::Settings::instance().send_presence);
    settings_view_->set_media_previews_pref(
        tesseract::Settings::instance().media_previews);
    settings_view_->set_invite_avatars_pref(
        tesseract::Settings::instance().invite_avatars);
    surface_->relayout();
}

void SettingsWidget::set_controller(tesseract::SettingsController* ctrl,
                                    const std::string& current_display_name)
{
    controller_ = ctrl;

    // Plug in the surface-relayout callback so DevicesSection's async
    // callbacks can invalidate the surface after mutating widgets.
    settings_view_->set_request_repaint([this]
    {
        if (surface_) surface_->relayout();
    });

    // Wire SettingsView (which wires AccountSection + DevicesSection).
    settings_view_->set_controller(ctrl);

    // Wire SettingsView avatar callbacks to controller.
    settings_view_->on_avatar_upload_requested = [this]
    {
        if (controller_) controller_->upload_avatar();
    };
    settings_view_->on_avatar_remove_requested = [this]
    {
        if (controller_) controller_->remove_avatar();
    };

    // Create (or recreate) the NativeTextField for name editing.
    name_field_ = surface_->host().make_text_field();
    name_field_->set_compact(true);
    name_field_->set_text(current_display_name);
    name_field_->set_placeholder("Display name");
    name_field_->set_visible(false);

    name_field_->set_on_submit(
        [this]
        {
            if (!controller_) return;
            const std::string text = name_field_->text();
            controller_->set_display_name(text);
            settings_view_->set_name_busy(true);
            surface_->relayout();
        });

    // Overwrite on_name_changed / on_name_result to also update the NativeTextField.
    ctrl->on_name_changed = [this](std::string name)
    {
        settings_view_->set_display_name_text(name);
        if (name_field_) name_field_->set_text(name);
        surface_->relayout();
    };

    ctrl->on_name_result = [this](bool ok, std::string error)
    {
        settings_view_->set_name_busy(false);
        if (!ok) settings_view_->set_name_error(std::move(error));
        surface_->relayout();
    };

    // Overwrite on_avatar_changed so the sidebar UserInfo strip can refresh.
    // The shared SettingsView lambda only updates the AccountSection chip.
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        settings_view_->set_avatar_url(mxc);
        surface_->relayout();
        if (on_local_avatar_changed) on_local_avatar_changed(std::move(mxc));
    };

    surface_->relayout();
}

void SettingsWidget::set_group_inactive_pref(bool enabled)
{
    settings_view_->set_group_inactive_pref(enabled);
}

void SettingsWidget::set_inactive_period_pref(int days)
{
    settings_view_->set_inactive_period_pref(days);
}

} // namespace gtk4
