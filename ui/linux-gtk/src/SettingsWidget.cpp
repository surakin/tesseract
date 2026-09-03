#include "SettingsWidget.h"
#include "tk/theme.h"
#include "views/settings/PronounsEditor.h"
#include "views/settings/TimezonePicker.h"

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

    // Everything else (theme/notifications/privacy/media/appearance toggles,
    // clear-caches, etc.) is wired generically by ShellBase::wire_settings_view_
    // — the shell calls that once on settings_view() after constructing this
    // widget. on_close/on_logout/on_reset_identity stay the shell's own
    // responsibility (each dismisses the settings surface differently), and
    // on_tab_changed needs this widget's own Surface.
    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

    surface_->set_root(std::move(view));

    // GTK's native GtkEntry needs compact mode for a snug visual fit inside
    // these rows — every other backend uses the default (non-compact) chrome.
    if (auto* f = settings_view_->name_field())      f->set_compact(true);
    if (auto* pe = settings_view_->pronouns_editor()) pe->set_compact(true);
    if (auto* f = settings_view_->tz_field())         f->set_compact(true);
    if (auto* f = settings_view_->bio_field())        f->set_compact(true);
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

    // set_controller() itself, the avatar upload/remove delegation, and the
    // sidebar-refreshing on_avatar_changed are wired generically by
    // ShellBase::wire_settings_controller_common_ (called by the shell right
    // after this), which takes relayout() below to invalidate this widget's
    // own Surface.

    surface_->relayout();
}

void SettingsWidget::relayout()
{
    if (surface_) surface_->relayout();
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
