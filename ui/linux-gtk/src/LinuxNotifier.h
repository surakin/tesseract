#pragma once
#include <tesseract/notifier.h>
#include <gio/gio.h>
#include <functional>
#include <string>
#include <unordered_map>

class LinuxNotifierGtk final : public tesseract::INotifier {
public:
    // Callback: (room_id, activation_token). Token is empty when unavailable;
    // non-empty on Wayland via the XDG Desktop Portal ActionInvoked signal.
    explicit LinuxNotifierGtk(
        std::function<void(std::string, std::string)> on_activate);
    ~LinuxNotifierGtk() override;
    void notify(const tesseract::Notification& n) override;

private:
    static void on_action_invoked_cb(GDBusConnection*, const char*, const char*,
                                     const char*, const char*,
                                     GVariant*, gpointer);
    static void on_notification_closed_cb(GDBusConnection*, const char*, const char*,
                                          const char*, const char*,
                                          GVariant*, gpointer);
    static void on_portal_action_invoked_cb(GDBusConnection*, const char*, const char*,
                                            const char*, const char*,
                                            GVariant*, gpointer);
    bool use_portal() const;

    GDBusConnection*                               bus_               = nullptr;
    guint                                          action_sub_        = 0;
    guint                                          closed_sub_        = 0;
    guint                                          portal_action_sub_ = 0;
    std::function<void(std::string, std::string)>  on_activate_;
    std::unordered_map<uint32_t, std::string>      id_to_room_;
    std::unordered_map<std::string, uint32_t>      room_to_id_;
    // Portal notifications use string IDs (sanitized room_id).
    std::unordered_map<std::string, std::string>   portal_id_to_room_;
};
