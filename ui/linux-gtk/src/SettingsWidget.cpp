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
    // Persisted directly here (self-contained — no extra wrapper/MainWindow
    // plumbing); the lock-screen privacy gate is always on regardless.
    settings_view_->on_image_previews_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_image_previews = e;
        s.save_to_disk(tesseract::config_dir());
    };

    surface_->set_root(std::move(view));
}

GtkWidget* SettingsWidget::widget() const
{
    return surface_->widget();
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
    settings_view_->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    surface_->relayout();
}

} // namespace gtk4
