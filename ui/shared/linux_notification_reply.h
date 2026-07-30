#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
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

} // namespace tesseract::linux_notify
