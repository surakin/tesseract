#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tesseract::linux_notify
{

// KDE Plasma's de-facto inline-reply convention (not part of the base
// freedesktop.org Notifications spec 1.2): an action literally named
// "inline-reply" plus optional x-kde-reply-* hints. GNOME Shell implements
// neither — clients must probe GetCapabilities() and gate on this string
// before offering the action. These are protocol-level literals; never
// localize kInlineReplyAction (it must match KDE's hardcoded string
// exactly). The hint *values* passed alongside these hint names are
// genuinely user-visible (rendered by Plasma's own notification widget) and
// should be localized via tk::tr() at the call site.
inline constexpr const char* kInlineReplyAction = "inline-reply";
inline constexpr const char* kHintPlaceholder = "x-kde-reply-placeholder-text";
inline constexpr const char* kHintSubmitLabel = "x-kde-reply-submit-button-text";

// Does a GetCapabilities() result support KDE's inline-reply convention?
// Pure/testable — no D-Bus dependency. Case-sensitive: KDE's capability
// string is always lowercase.
inline bool supports_inline_reply(const std::vector<std::string>& caps)
{
    return std::find(caps.begin(), caps.end(), kInlineReplyAction) !=
           caps.end();
}

// XDG Desktop Portal (org.freedesktop.portal.Notification) inline-reply
// convention, standardized since interface version 2 — unlike the legacy
// D-Bus KDE extension above, this one is genuinely part of the portal spec
// (see org.freedesktop.portal.Notification.xml's "im.received" category
// docs), so it's not gated on a per-daemon capability string; only on the
// portal interface's own "version" property being >= this minimum.
inline constexpr std::uint32_t kPortalReplyMinVersion = 2;
// Notification-level "category" that enables the im.reply-with-text button
// purpose. The button's `label` IS genuinely rendered by the compositor's
// own reply widget (unlike the legacy kInlineReplyAction's ignored label),
// so it should be localized via tk::tr() at the call site.
inline constexpr const char* kPortalImCategory = "im.received";
inline constexpr const char* kPortalReplyPurpose = "im.reply-with-text";
// Our own action id for the reply button. Deliberately NOT prefixed with
// "app." — that prefix means "exported, activate via ActivateAction()",
// which delivers the reply text as the second element of that call's
// parameter list instead of via the ActionInvoked signal. Using a plain,
// non-exported name means the daemon fires ActionInvoked(id, action,
// parameter) instead, with the typed reply text as parameter[2].
inline constexpr const char* kPortalReplyAction = "reply";

// Identity of a live (not-yet-closed) notification, keyed by the daemon-
// assigned notification id. NotificationReplied only reports back
// (id, text), so both room_id and event_id must already be sitting in the
// id-keyed map from Notify() time.
struct RepliableNotification
{
    std::string room_id;
    std::string event_id;
};

// Bookkeeping shared verbatim between LinuxNotifierQt and LinuxNotifierGtk:
// correlating a daemon-assigned notification id (or, on the portal path, our
// own string id) back to a room/event, and stashing an ActivationToken until
// the ActionInvoked that consumes it arrives. Pure C++, no Qt/GLib — each
// notifier's D-Bus signal handler just unmarshals its own library's argument
// types down to plain values and calls into this. Deliberately composition,
// not a base class: there's no polymorphism here, just identical bookkeeping
// two otherwise-unrelated classes both happen to need. Notification content
// marshaling (Notify()/AddNotification() argument construction, the shape of
// D-Bus signal parameters) is NOT included here and stays duplicated in each
// notifier — that's genuine platform-specific marshaling code, not logic, and
// pulling QDBusInterface/GDBusConnection types into ui/shared/ would break
// its platform-neutrality (see linux_portal.h for the established precedent).
class NotificationCorrelation
{
public:
    void record(std::uint32_t id, std::string room_id, std::string event_id)
    {
        id_to_room_[id] = {std::move(room_id), std::move(event_id)};
    }

    void record_portal(std::string portal_id, std::string room_id,
                       std::string event_id)
    {
        // Portal ids are caller-chosen and reused per room (each new
        // notification for the same room replaces the prior one), so this
        // just overwrites with the latest event_id — same map either way.
        portal_id_to_room_[std::move(portal_id)] =
            {std::move(room_id), std::move(event_id)};
    }

    std::optional<RepliableNotification> find(std::uint32_t id) const
    {
        auto it = id_to_room_.find(id);
        if (it == id_to_room_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<RepliableNotification>
    find_portal(const std::string& portal_id) const
    {
        auto it = portal_id_to_room_.find(portal_id);
        if (it == portal_id_to_room_.end()) return std::nullopt;
        return it->second;
    }

    // Stash a token delivered by ActivationToken, ahead of the ActionInvoked
    // that will consume it (ActivationToken always arrives first for the
    // same id, when the daemon supports it at all).
    void stash_token(std::uint32_t id, std::string token)
    {
        pending_tokens_[id] = std::move(token);
    }

    // Consume (erase-and-return) a pending token. Empty if the daemon never
    // sent one — every caller must treat that as "no token", not an error.
    std::string take_token(std::uint32_t id)
    {
        auto it = pending_tokens_.find(id);
        if (it == pending_tokens_.end()) return {};
        std::string token = std::move(it->second);
        pending_tokens_.erase(it);
        return token;
    }

    // Called on NotificationClosed: forget everything keyed by this id. Only
    // id_to_room_/pending_tokens_ — portal_id_to_room_ is keyed by our own
    // string id and portal notifications don't fire NotificationClosed.
    void forget(std::uint32_t id)
    {
        id_to_room_.erase(id);
        pending_tokens_.erase(id);
    }

private:
    std::unordered_map<std::uint32_t, RepliableNotification> id_to_room_;
    std::unordered_map<std::string, RepliableNotification> portal_id_to_room_;
    std::unordered_map<std::uint32_t, std::string> pending_tokens_;
};

} // namespace tesseract::linux_notify
