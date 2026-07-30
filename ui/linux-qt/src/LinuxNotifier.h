#pragma once
#include <tesseract/notifier.h>
#include "../../shared/linux_notification_reply.h"
#include <QDBusInterface>
#include <QObject>
#include <functional>
#include <string>
#include <unordered_map>

class LinuxNotifierQt final : public QObject, public tesseract::INotifier
{
    Q_OBJECT
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
    explicit LinuxNotifierQt(
        std::function<void(std::string, std::string)> on_activate,
        std::function<void(std::string, std::string, std::string)> on_reply,
        QObject* parent = nullptr);
    ~LinuxNotifierQt() override = default;
    void notify(const tesseract::Notification& n) override;

private slots:
    void onActionInvoked(uint id, const QString& action);
    void onNotificationClosed(uint id, uint reason);
    void onPortalActionInvoked(const QString& notification_id,
                               const QString& action,
                               const QVariantList& parameter);
    void onNotificationReplied(uint id, const QString& text);

private:
    bool use_portal() const;

    QDBusInterface iface_;
    QDBusInterface portal_;
    std::function<void(std::string, std::string)> on_activate_;
    std::function<void(std::string, std::string, std::string)> on_reply_;
    // Both probed ONCE, synchronously, in the constructor — deliberately NOT
    // a lazily-computed function-local static. A blocking QDBusInterface
    // call pumps the Qt event loop while it waits for the reply; if that
    // pump dispatches a *different* account's queued notify() call before
    // the first probe's magic-static initialization completes, the second
    // call reenters the same not-yet-initialized static from the same
    // thread — undefined behavior (observed in practice as a permanent
    // wedge: notify() calls stop reaching the daemon at all, for every
    // account, for the rest of the process). Doing the probe once up front,
    // before this object is reachable from any notify() call, avoids the
    // hazard entirely; the minor per-account redundancy is a fair trade.
    bool legacy_reply_supported_ = false;
    bool portal_reply_supported_ = false;
    std::unordered_map<uint32_t, tesseract::linux_notify::RepliableNotification>
        id_to_room_;
    // Portal notifications use string IDs (sanitized room_id); each new
    // notification for the same room reuses the same id (replacing the
    // prior one), so only the latest event_id per room needs tracking here.
    std::unordered_map<std::string, tesseract::linux_notify::RepliableNotification>
        portal_id_to_room_;
};
