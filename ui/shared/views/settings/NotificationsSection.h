#pragma once

// Settings panel section: three labelled checkbox rows under a "Notifications"
// header —
//   1. "Enable notifications on this device"
//   2. "Hide message content in notifications"
//   3. "Show image & sticker previews in notifications"
// Reads initial state from Settings::instance() and fires the matching
// callback when a row is toggled. (The lock-screen privacy gate is always on
// regardless of row 3 — see ShellBase::notification_image_allowed_.)
//
// A second "Mentions & Keywords" group holds the account-wide, push-rule-
// backed granular controls (Element-style): separate toggles for @-mentions,
// @room mentions, and general messages, plus an editable keyword list. Unlike
// the local-pref rows above, these read/write server-side push rules, so
// state is fetched asynchronously (see set_loading()) and each toggle's
// setter can fail server-side — on failure the caller reverts the switch via
// set_mentions_enabled()/set_room_mentions_enabled()/set_notify_all_messages()
// rather than trusting the click.

#include "SettingsPage.h"

#include "tk/controls.h"

#include <functional>
#include <string>
#include <vector>

namespace tk
{
class TextField;
} // namespace tk

namespace tesseract::views
{

class SettingsGroup;

class NotificationsSection : public SettingsPage
{
public:
    NotificationsSection();
    ~NotificationsSection() override;

    // Silently update checkbox state without firing callbacks.
    void set_checked(bool enabled);                // row 1
    void set_hide_content_checked(bool enabled);   // row 2
    void set_image_previews_checked(bool enabled); // row 3

    // Fire with the new boolean state when the matching row is toggled.
    std::function<void(bool)> on_notifications_changed;
    std::function<void(bool)> on_hide_content_changed;
    std::function<void(bool)> on_image_previews_changed;

    // ---- Mentions & Keywords (push-rule-backed, account-wide) ----

    // Silent state sync — used both for the initial async fetch and for
    // reverting a toggle after a failed server write.
    void set_mentions_enabled(bool enabled);
    void set_room_mentions_enabled(bool enabled);
    void set_notify_all_messages(bool enabled);
    // Master toggle above the keyword list — enables/disables every keyword
    // rule at once without touching the keyword list itself.
    void set_notify_on_keywords(bool enabled);
    // Baseline load only (e.g. the initial async fetch on Settings open) —
    // replaces the whole list. A single add/remove click applies optimistic
    // add_keyword_row_()/remove_keyword_row_() below rather than going
    // through here, since re-fetching the server list right after a mutation
    // races with matrix-sdk's own /sync-driven push-rules cache (see
    // SettingsController::add_notification_keyword's comment).
    void set_notification_keywords(std::vector<std::string> keywords);

    // Undo an optimistic add/remove after the server write comes back
    // ok == false. No-op if `keyword` isn't in the expected state already
    // (e.g. a stale/duplicate revert).
    void revert_keyword_add(const std::string& keyword);
    void revert_keyword_remove(const std::string& keyword);

    // Toggles the "Loading…" placeholder shown in place of the three
    // switches/keyword list while the initial fetch is in flight.
    void set_mentions_loading(bool loading);

    // Inline error text under the keyword add-row (e.g. "Could not add
    // keyword"). Empty clears it.
    void set_keyword_error(std::string error);

    // Fired on toggle/click. Callers must eventually call the matching
    // set_* above with the authoritative post-write value (revert on
    // failure).
    std::function<void(bool)> on_mentions_changed;
    std::function<void(bool)> on_room_mentions_changed;
    std::function<void(bool)> on_notify_all_messages_changed;
    std::function<void(bool)> on_notify_on_keywords_changed;
    // Fired with the trimmed, non-empty text from the add-keyword field.
    std::function<void(std::string)> on_keyword_add_requested;
    std::function<void(std::string)> on_keyword_remove_requested;

private:
    class KeywordChip; // one removable chip — defined in NotificationsSection.cpp
    class KeywordFlow; // wraps chips left-to-right, onto new rows as needed
    void rebuild_keyword_rows_();
    void try_submit_keyword_();

    tk::CheckButton* notif_cb_        = nullptr; // row 1
    tk::CheckButton* hide_content_cb_ = nullptr; // row 2
    tk::CheckButton* previews_cb_     = nullptr; // row 3

    SettingsGroup*    mentions_group_       = nullptr;
    tk::SwitchButton* mentions_switch_      = nullptr;
    tk::SwitchButton* room_mentions_switch_ = nullptr;
    tk::SwitchButton* all_messages_switch_  = nullptr;
    tk::SwitchButton* notify_on_keywords_switch_ = nullptr;
    tk::Label*        mentions_loading_label_ = nullptr;

    tk::TextField* keyword_field_    = nullptr;
    tk::Button*    keyword_add_btn_  = nullptr;
    KeywordFlow*   keyword_flow_     = nullptr; // owned by mentions_group_
    tk::Label*     keyword_error_label_ = nullptr;

    std::vector<std::string>    keywords_;
    std::vector<KeywordChip*>   keyword_chips_; // borrowed; owned by keyword_flow_
    bool mentions_loading_ = false;
};

} // namespace tesseract::views
