#include "SettingsWidget.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QResizeEvent>

#include "tk/theme.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace qt6
{

SettingsWidget::SettingsWidget(QWidget* parent)
    : QWidget(parent), surface_(new tk::qt6::Surface(tk::Theme::light(), this))
{
    auto view = std::make_unique<tesseract::views::SettingsView>(
        &surface_->host());
    settings_view_ = view.get();

    settings_view_->on_close = [this]
    {
        emit settingsClosed();
    };
    settings_view_->on_logout = [this]
    {
        emit settingsClosed();
        emit logoutRequested();
    };
    settings_view_->on_theme_preference_changed =
        [this](tesseract::Settings::ThemePreference pref)
    {
        emit themeChanged(pref);
    };
    settings_view_->on_notifications_changed = [this](bool enabled)
    {
        emit notificationsChanged(enabled);
    };
    settings_view_->on_send_presence_changed = [this](bool enabled)
    {
        emit presenceChanged(enabled);
    };
    settings_view_->on_index_messages_changed = [this](bool enabled)
    {
        emit indexMessagesChanged(enabled);
    };
#ifdef TESSERACT_GITHUB_REPO
    settings_view_->on_check_for_updates_changed = [this](bool enabled)
    {
        emit checkForUpdatesChanged(enabled);
    };
#endif
    settings_view_->on_media_previews_changed =
        [this](tesseract::Settings::MediaPreviews mode)
    {
        emit mediaPreviewsChanged(mode);
    };
    settings_view_->on_invite_avatars_changed = [this](bool enabled)
    {
        emit inviteAvatarsChanged(enabled);
    };
    // Persisted directly here (self-contained — no extra wrapper/MainWindow
    // plumbing); the lock-screen privacy gate is always on regardless.
    settings_view_->on_hide_content_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_hide_content = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_image_previews_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_image_previews = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_prefetch_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.prefetch_full_media = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_group_inactive_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.group_inactive_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
        emit roomListGroupingChanged();
    };
    settings_view_->on_group_unread_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.group_unread_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
        emit roomListGroupingChanged();
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        auto& s = tesseract::Settings::instance();
        s.inactive_room_threshold_days = days;
        s.save_to_disk(tesseract::config_dir());
        emit roomListGroupingChanged();
    };
    settings_view_->on_autoscroll_unread_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.autoscroll_unread_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_show_membership_events_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.show_room_join_leave_events = enabled;
        s.save_to_disk(tesseract::config_dir());
        emit membershipEventsPrefChanged(enabled);
    };
    settings_view_->on_msc2545_legacy_compat_changed = [this](bool enabled)
    {
        emit msc2545LegacyCompatChanged(enabled);
    };

    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

    settings_view_->on_clear_caches = [this] { emit clearCachesRequested(); };

    settings_view_->on_reset_identity = [this] { emit resetIdentityRequested(); };

    surface_->set_root(std::move(view));
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

void SettingsWidget::set_show_membership_events_pref(bool enabled)
{
    settings_view_->set_show_membership_events_pref(enabled);
}

void SettingsWidget::set_msc2545_legacy_compat_pref(bool enabled)
{
    settings_view_->set_msc2545_legacy_compat_pref(enabled);
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
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
        surface_->relayout();
    }
}

void SettingsWidget::update_anim_regions()
{
    if (surface_)
        surface_->update_anim_regions();
}

void SettingsWidget::request_repaint()
{
    if (surface_)
        surface_->update();
}

void SettingsWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (surface_)
    {
        surface_->setGeometry(0, 0, width(), height());
    }
}

void SettingsWidget::set_controller(tesseract::SettingsController* ctrl)
{
    controller_ = ctrl;

    // Wire SettingsView (which wires AccountSection + DevicesSection).
    // Plug in the surface-relayout callback so the section's async device
    // callbacks can invalidate the surface after mutating widgets.
    settings_view_->set_request_repaint([this]
    {
        if (surface_) surface_->relayout();
    });
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

    // Room key export/import dialog callbacks.
    ctrl->show_passphrase_prompt =
        [this](std::string title, std::function<void(std::string)> cb)
    {
        bool ok = false;
        QString pass = QInputDialog::getText(
            this, QString::fromStdString(title), "Passphrase:",
            QLineEdit::Password, "", &ok);
        if (ok && !pass.isEmpty())
            cb(pass.toStdString());
    };
    ctrl->show_save_file_dialog =
        [this](std::string suggested, std::function<void(std::string)> cb)
    {
        QString path = QFileDialog::getSaveFileName(
            this, "Export Room Keys", QString::fromStdString(suggested));
        if (!path.isEmpty())
            cb(path.toStdString());
    };
    ctrl->show_open_file_dialog =
        [this](std::function<void(std::string)> cb)
    {
        QString path = QFileDialog::getOpenFileName(this, "Import Room Keys");
        if (!path.isEmpty())
            cb(path.toStdString());
    };
    ctrl->on_export_keys_result = [this](bool ok, std::string error)
    {
        if (ok)
            QMessageBox::information(this, "Export complete",
                                     "Room keys exported successfully.");
        else
            QMessageBox::warning(this, "Export failed",
                                 QString::fromStdString(error));
    };
    ctrl->on_import_keys_result = [this](bool ok, std::string error)
    {
        if (ok)
            QMessageBox::information(this, "Import complete",
                                     "Room keys imported successfully.");
        else
            QMessageBox::warning(this, "Import failed",
                                 QString::fromStdString(error));
    };

    // The name field's on_submit (and rename result push-back) is wired by
    // SettingsView::set_controller() above via AccountSection::name_field().
    // Only the sidebar UserInfo strip refresh is shell-specific, so
    // on_avatar_changed still needs overwriting here.
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        settings_view_->set_avatar_url(mxc);
        surface_->relayout();
        emit localAvatarChanged(QString::fromStdString(mxc));
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

} // namespace qt6
