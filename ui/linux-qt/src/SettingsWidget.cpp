#include "SettingsWidget.h"

#include <QResizeEvent>

#include "tk/theme.h"

namespace qt6 {

SettingsWidget::SettingsWidget(QWidget* parent)
    : QWidget(parent),
      surface_(new tk::qt6::Surface(tk::Theme::light(), this))
{
    auto view = std::make_unique<tesseract::views::SettingsView>();
    settings_view_ = view.get();

    settings_view_->on_close = [this]
    {
        emit settingsClosed();
    };
    settings_view_->on_theme_changed = [this](tesseract::Settings::ThemePreference pref)
    {
        emit themeChanged(pref);
    };
    settings_view_->on_notifications_changed = [this](bool enabled)
    {
        emit notificationsChanged(enabled);
    };

    surface_->set_root(std::move(view));
}

void SettingsWidget::populate(std::string display_name,
                              std::string user_id,
                              std::string avatar_mxc,
                              tesseract::views::AccountSection::ImageProvider provider,
                              tesseract::Settings::ThemePreference theme_pref,
                              bool notifications_enabled)
{
    settings_view_->set_account_info(std::move(display_name),
                                     std::move(user_id),
                                     std::move(avatar_mxc));
    settings_view_->set_image_provider(std::move(provider));
    settings_view_->set_theme_pref(theme_pref);
    settings_view_->set_notifications_enabled(notifications_enabled);
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
