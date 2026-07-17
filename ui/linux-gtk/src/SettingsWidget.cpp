#include "SettingsWidget.h"
#include "tk/theme.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace gtk4
{

SettingsWidget::SettingsWidget()
    : surface_(std::make_unique<tk::gtk4::Surface>(tk::Theme::light()))
{
    auto view = tk::create_root_widget<tesseract::views::SettingsView>(
        &surface_->host());
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
    settings_view_->on_theme_preference_changed = [this](auto p)
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
    settings_view_->on_index_messages_changed = [this](bool e)
    {
        if (on_index_messages_changed)
            on_index_messages_changed(e);
    };
#ifdef TESSERACT_GITHUB_REPO
    settings_view_->on_check_for_updates_changed = [this](bool e)
    {
        if (on_check_for_updates_changed)
            on_check_for_updates_changed(e);
    };
#endif
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
    settings_view_->on_group_unread_changed = [this](bool v)
    {
        if (on_group_unread_changed)
            on_group_unread_changed(v);
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed)
            on_inactive_period_changed(days);
    };
    settings_view_->on_autoscroll_unread_changed = [this](bool v)
    {
        if (on_autoscroll_unread_changed)
            on_autoscroll_unread_changed(v);
    };
    settings_view_->on_show_membership_events_changed = [this](bool v)
    {
        if (on_show_membership_events_changed)
            on_show_membership_events_changed(v);
    };
    settings_view_->on_msc2545_legacy_compat_changed = [this](bool v)
    {
        if (on_msc2545_legacy_compat_changed)
            on_msc2545_legacy_compat_changed(v);
    };
    settings_view_->on_developer_mode_changed = [this](bool v)
    {
        if (on_developer_mode_changed)
            on_developer_mode_changed(v);
    };
    settings_view_->on_send_maps_urls_as_location_changed = [this](bool v)
    {
        if (on_send_maps_urls_as_location_changed)
            on_send_maps_urls_as_location_changed(v);
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

    settings_view_->on_reset_identity = [this]
    {
        if (on_reset_identity) on_reset_identity();
    };

    surface_->set_root(std::move(view));

    // GTK's native GtkEntry needs compact mode for a snug visual fit inside
    // these rows — every other backend uses the default (non-compact) chrome.
    if (auto* f = settings_view_->name_field())     f->set_compact(true);
    if (auto* f = settings_view_->pronouns_field()) f->set_compact(true);
    if (auto* f = settings_view_->tz_field())       f->set_compact(true);
    if (auto* f = settings_view_->bio_field())      f->set_compact(true);
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
                                     uint64_t memory_bytes,
                                     uint64_t mem_hits, uint64_t mem_misses,
                                     uint64_t disk_hits, uint64_t disk_misses)
{
    if (settings_view_)
        settings_view_->set_cache_sizes(local_bytes, sdk_bytes, memory_bytes,
                                        mem_hits, mem_misses,
                                        disk_hits, disk_misses);
}

void SettingsWidget::set_theme(const tk::Theme& t)
{
    surface_->set_theme(t);
    surface_->root()->apply_theme(t);
    surface_->relayout();
}

void SettingsWidget::update_anim_regions()
{
    if (surface_)
        surface_->relayout();
}

void SettingsWidget::request_repaint()
{
    if (surface_)
        gtk_widget_queue_draw(surface_->widget());
}

void SettingsWidget::populate(
    std::string display_name, std::string user_id, std::string avatar_mxc,
    tesseract::views::AccountSection::ImageProvider provider)
{
    settings_view_->set_account_info(std::move(display_name),
                                     std::move(user_id), std::move(avatar_mxc));
    settings_view_->set_image_provider(std::move(provider));
    settings_view_->load_persisted_settings();
    surface_->relayout();
}

void SettingsWidget::set_controller(tesseract::SettingsController* ctrl)
{
    controller_ = ctrl;

    // Plug in the surface-relayout callback so DevicesSection's async
    // callbacks can invalidate the surface after mutating widgets.
    settings_view_->set_request_repaint([this]
    {
        if (surface_) surface_->relayout();
    });

    // Wire SettingsView (which wires AccountSection + DevicesSection, and —
    // via AccountSection::name_field()/pronouns_field()/tz_field()/
    // bio_field() — the four self-owned fields' on_submit handlers).
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

void SettingsWidget::set_extended_profile(const tesseract::ExtendedProfile& profile)
{
    if (settings_view_)
        settings_view_->set_extended_profile(profile);
    if (surface_) surface_->relayout();
}

void SettingsWidget::set_profile_field_busy(const std::string& key, bool busy)
{
    if (settings_view_)
        settings_view_->set_profile_field_busy(key, busy);
    if (surface_)
        surface_->relayout();
}

void SettingsWidget::set_profile_field_error(const std::string& key,
                                              std::string error)
{
    if (settings_view_)
        settings_view_->set_profile_field_error(key, std::move(error));
    if (surface_)
        surface_->relayout();
}

} // namespace gtk4
