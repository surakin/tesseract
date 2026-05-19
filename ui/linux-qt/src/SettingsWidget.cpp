#include "SettingsWidget.h"

#include <QResizeEvent>

#include "tk/theme.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

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
    settings_view_->on_theme_changed =
        [this](tesseract::Settings::ThemePreference pref)
    {
        emit themeChanged(pref);
    };
    settings_view_->on_notifications_changed = [this](bool enabled)
    {
        emit notificationsChanged(enabled);
    };
    // Persisted directly here (self-contained — no extra wrapper/MainWindow
    // plumbing); the lock-screen privacy gate is always on regardless.
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

    surface_->set_root(std::move(view));
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
    settings_view_->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    settings_view_->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    surface_->relayout();
}

void SettingsWidget::set_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->relayout();
    }
}

void SettingsWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (surface_)
    {
        surface_->setGeometry(0, 0, width(), height());
    }
}

} // namespace qt6
