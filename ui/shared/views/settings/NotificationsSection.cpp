#include "NotificationsSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"

#include <memory>

namespace tesseract::views
{

NotificationsSection::NotificationsSection()
{
    const auto& s = tesseract::Settings::instance();

    auto* group = add_group("Notifications");

    auto notif_cb = std::make_unique<tk::CheckButton>(
        "Enable notifications on this device", s.notifications_enabled);
    notif_cb_ = group->add_widget(std::move(notif_cb));
    notif_cb_->on_change = [this](bool v)
    {
        if (on_notifications_changed) on_notifications_changed(v);
    };

    auto hide_content_cb = std::make_unique<tk::CheckButton>(
        "Hide message content in notifications",
        s.notification_hide_content);
    hide_content_cb_ = group->add_widget(std::move(hide_content_cb));
    hide_content_cb_->on_change = [this](bool v)
    {
        if (on_hide_content_changed) on_hide_content_changed(v);
    };

    auto previews_cb = std::make_unique<tk::CheckButton>(
        "Show image & sticker previews in notifications",
        s.notification_image_previews);
    previews_cb_ = group->add_widget(std::move(previews_cb));
    previews_cb_->on_change = [this](bool v)
    {
        if (on_image_previews_changed) on_image_previews_changed(v);
    };
}

void NotificationsSection::set_checked(bool enabled)
{
    notif_cb_->set_checked(enabled);
}

void NotificationsSection::set_hide_content_checked(bool enabled)
{
    hide_content_cb_->set_checked(enabled);
}

void NotificationsSection::set_image_previews_checked(bool enabled)
{
    previews_cb_->set_checked(enabled);
}

} // namespace tesseract::views
