#include "SettingsWidget.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QResizeEvent>
#include <QToolTip>

#include "tk/theme.h"
#include "tk/i18n.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace
{

// Minimal JSON string escaper for profile field values.
std::string json_quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s)
    {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
}

} // namespace

namespace qt6
{

SettingsWidget::SettingsWidget(QWidget* parent)
    : QWidget(parent), surface_(new tk::qt6::Surface(tk::Theme::light(), this))
{
    auto view = std::make_unique<tesseract::views::SettingsView>();
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
    settings_view_->on_theme_changed =
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

    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

    settings_view_->on_clear_caches = [this] { emit clearCachesRequested(); };

    settings_view_->on_reset_identity = [this] { emit resetIdentityRequested(); };

    {
        QPointer<tk::qt6::Surface> sfp = surface_;
        settings_view_->on_show_tooltip =
            [sfp](std::string text, tk::Rect anchor)
        {
            if (!sfp)
                return;
            QPoint local(static_cast<int>(anchor.x),
                         static_cast<int>(anchor.y + anchor.h));
            QToolTip::showText(sfp->mapToGlobal(local),
                               QString::fromStdString(text), sfp);
        };
        settings_view_->on_hide_tooltip = []
        {
            QToolTip::hideText();
        };
    }

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
            if (pronouns_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->pronouns_field_rect();
                pronouns_field_->set_visible(!r.empty());
                if (!r.empty())
                    pronouns_field_->set_rect(r);
            }
            if (tz_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->tz_field_rect();
                tz_field_->set_visible(!r.empty());
                if (!r.empty())
                    tz_field_->set_rect(r);
            }
            if (bio_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->bio_field_rect();
                bio_field_->set_visible(!r.empty());
                if (!r.empty())
                    bio_field_->set_rect(r);
            }
        });
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
    settings_view_->set_index_messages_pref(
        tesseract::Settings::instance().index_messages_for_search);
#ifdef TESSERACT_GITHUB_REPO
    settings_view_->set_check_for_updates_pref(
        tesseract::Settings::instance().check_for_updates);
#endif
    settings_view_->set_media_previews_pref(
        tesseract::Settings::instance().media_previews);
    settings_view_->set_invite_avatars_pref(
        tesseract::Settings::instance().invite_avatars);
    settings_view_->set_group_inactive_pref(
        tesseract::Settings::instance().group_inactive_rooms);
    settings_view_->set_group_unread_pref(
        tesseract::Settings::instance().group_unread_rooms);
    settings_view_->set_inactive_period_pref(
        tesseract::Settings::instance().inactive_room_threshold_days);
    settings_view_->set_autoscroll_unread_pref(
        tesseract::Settings::instance().autoscroll_unread_rooms);
    settings_view_->set_show_membership_events_pref(
        tesseract::Settings::instance().show_room_join_leave_events);
    surface_->relayout();
}

void SettingsWidget::set_show_membership_events_pref(bool enabled)
{
    settings_view_->set_show_membership_events_pref(enabled);
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

void SettingsWidget::set_controller(tesseract::SettingsController* ctrl,
                                    const std::string& current_display_name)
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

    // Create (or recreate) the NativeTextField for name editing.
    name_field_ = surface_->host().make_text_field();
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
        emit localAvatarChanged(QString::fromStdString(mxc));
    };

    // Create NativeTextField overlays for the three extended profile fields.
    static constexpr char kKeyPronouns[] = "io.fsky.nyx.pronouns";
    static constexpr char kKeyTz[]       = "us.cloke.msc4175.tz";
    static constexpr char kKeyBio[]      = "gay.fomx.biography";

    pronouns_field_ = surface_->host().make_text_field();
    pronouns_field_->set_placeholder(tk::tr("Pronouns"));
    pronouns_field_->set_visible(false);
    pronouns_field_->set_on_submit(
        [this]
        {
            const std::string text = pronouns_field_->text();
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "[{\"summary\":" + json_quote(text) +
                             ",\"language\":\"en\"}]";
            settings_view_->set_profile_field_busy(kKeyPronouns, true);
            emit profileFieldChanged(
                QString(kKeyPronouns), QString::fromStdString(value_json));
            surface_->relayout();
        });

    tz_field_ = surface_->host().make_text_field();
    tz_field_->set_placeholder(tk::tr("Timezone (e.g. Europe/London)"));
    tz_field_->set_visible(false);
    tz_field_->set_on_submit(
        [this]
        {
            const std::string text = tz_field_->text();
            std::string value_json = text.empty() ? "null" : json_quote(text);
            settings_view_->set_profile_field_busy(kKeyTz, true);
            emit profileFieldChanged(
                QString(kKeyTz), QString::fromStdString(value_json));
            surface_->relayout();
        });

    bio_field_ = surface_->host().make_text_field();
    bio_field_->set_placeholder(tk::tr("Short biography"));
    bio_field_->set_visible(false);
    bio_field_->set_on_submit(
        [this]
        {
            const std::string text = bio_field_->text();
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "{\"m.text\":[{\"body\":" + json_quote(text) + "}]}";
            settings_view_->set_profile_field_busy(kKeyBio, true);
            emit profileFieldChanged(
                QString(kKeyBio), QString::fromStdString(value_json));
            surface_->relayout();
        });

    surface_->relayout();
}

void SettingsWidget::set_extended_profile(const tesseract::ExtendedProfile& profile)
{
    if (settings_view_)
        settings_view_->set_extended_profile(profile);
    if (pronouns_field_) pronouns_field_->set_text(profile.pronouns);
    if (tz_field_)       tz_field_->set_text(profile.tz);
    if (bio_field_)      bio_field_->set_text(profile.biography);
    if (surface_)        surface_->relayout();
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
