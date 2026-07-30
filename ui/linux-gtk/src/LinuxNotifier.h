#pragma once
#include <tesseract/notifier.h>
#include "../../shared/linux_notification_reply.h"
#include <gio/gio.h>
#include <functional>
#include <string>

class LinuxNotifierGtk final : public tesseract::INotifier
{
public:
    // on_activate: (room_id, activation_token). Token is empty when
    // unavailable; non-empty on Wayland via the XDG Desktop Portal
    // ActionInvoked signal.
    // on_reply: (room_id, event_id, reply_text), fired from either the
    // KDE-only legacy D-Bus "inline-reply" extension, or the portal's
    // standardized "im.reply-with-text" button purpose (interface v2+) —
    // see ui/shared/linux_notification_reply.h for both. Never fires when
    // neither is supported. Deliberately separate from on_activate — a
    // reply must not switch account or navigate, unlike a click.
    explicit LinuxNotifierGtk(
        std::function<void(std::string, std::string)> on_activate,
        std::function<void(std::string, std::string, std::string)> on_reply);
    ~LinuxNotifierGtk() override;
    void notify(const tesseract::Notification& n) override;

private:
    static void on_action_invoked_cb(GDBusConnection*, const char*, const char*,
                                     const char*, const char*, GVariant*,
                                     gpointer);
    static void on_notification_closed_cb(GDBusConnection*, const char*,
                                          const char*, const char*, const char*,
                                          GVariant*, gpointer);
    static void on_portal_action_invoked_cb(GDBusConnection*, const char*,
                                            const char*, const char*,
                                            const char*, GVariant*, gpointer);
    static void on_notification_replied_cb(GDBusConnection*, const char*,
                                           const char*, const char*,
                                           const char*, GVariant*, gpointer);
    // KDE/GNOME de-facto extension (not in the base freedesktop.org spec):
    // fires just before ActionInvoked with a Wayland xdg_activation_v1
    // token, provided notify() sends a "desktop-entry" hint the daemon can
    // resolve. Daemons that don't implement it simply never fire this, so
    // subscribing unconditionally is safe. Stashed in correlation_ until
    // the matching ActionInvoked arrives.
    static void on_activation_token_cb(GDBusConnection*, const char*,
                                       const char*, const char*, const char*,
                                       GVariant*, gpointer);
    bool use_portal() const;

    GDBusConnection* bus_ = nullptr;
    guint action_sub_ = 0;
    guint closed_sub_ = 0;
    guint portal_action_sub_ = 0;
    guint replied_sub_ = 0;
    guint activation_token_sub_ = 0;
    std::function<void(std::string, std::string)> on_activate_;
    std::function<void(std::string, std::string, std::string)> on_reply_;
    // Both probed ONCE, synchronously, in the constructor — deliberately NOT
    // a lazily-computed function-local static. The Qt6 notifier's original
    // version of this pattern (a blocking D-Bus call inside a magic-static
    // initializer) caused a permanent wedge in practice: a blocking call can
    // pump the calling event loop, and if that dispatches a *different*
    // account's queued notify() call before the first probe's static
    // finishes initializing, the second call reenters the same
    // not-yet-initialized static from the same thread — undefined behavior.
    // Probing once up front, before this object is reachable from any
    // notify() call, avoids the hazard entirely.
    bool legacy_reply_supported_ = false;
    bool portal_reply_supported_ = false;
    // Notification-id <-> room/event correlation and pending activation
    // tokens, shared verbatim with LinuxNotifierQt — see its definition for
    // why this is composition rather than a base class.
    tesseract::linux_notify::NotificationCorrelation correlation_;
};
