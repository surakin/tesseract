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
    auto view = tk::create_root_widget<tesseract::views::SettingsView>(
        &surface_->host());
    settings_view_ = view.get();

    // Everything else (theme/notifications/privacy/media/appearance toggles,
    // clear-caches, etc.) is wired generically by ShellBase::wire_settings_view_
    // — the shell calls that once on settings_view() after constructing this
    // widget. on_close/on_logout/on_reset_identity stay the shell's own
    // responsibility (each dismisses the settings surface differently, via
    // these Qt signals), and on_tab_changed needs this widget's own Surface.
    settings_view_->on_close = [this]
    {
        emit settingsClosed();
    };
    settings_view_->on_logout = [this]
    {
        emit settingsClosed();
        emit logoutRequested();
    };
    settings_view_->on_reset_identity = [this] { emit resetIdentityRequested(); };

    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

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

void SettingsWidget::set_launch_at_login_pref(bool enabled)
{
    settings_view_->set_launch_at_login_pref(enabled);
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

    // Plug in the surface-relayout callback so the section's async device
    // callbacks can invalidate the surface after mutating widgets. Everything
    // else — settings_view_->set_controller() itself, avatar upload/remove
    // delegation, and the sidebar-refreshing on_avatar_changed — is wired
    // generically by ShellBase::wire_settings_controller_common_ (called by
    // the shell right after this), which takes relayout() below to
    // invalidate this widget's own Surface.
    settings_view_->set_request_repaint([this]
    {
        if (surface_) surface_->relayout();
    });

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

} // namespace qt6
