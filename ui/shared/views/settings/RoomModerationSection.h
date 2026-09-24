#pragma once

// The "Moderation" tab of RoomSettingsView. Currently holds the room's
// banned-users list, each row with an "Unban" button. Like the Bridge tab,
// actions here apply immediately rather than being staged for
// Accept/Cancel: an unban is its own server-side operation, not a state
// field of the settings form.

#include "SettingsPage.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tk
{
class Label;
} // namespace tk

namespace tesseract::views
{

class SettingsGroup;

class RoomModerationSection : public SettingsPage
{
public:
    RoomModerationSection();
    ~RoomModerationSection() override = default;

    // Show "Loading…" in place of the list (the fetch syncs the member list
    // first, so it can take a moment). Cleared by set_banned_members().
    void set_loading(bool loading);

    // Replace the list. Empty → "No banned users" label, no rows. Clears
    // loading state and every row's pending-unban state.
    void set_banned_members(std::vector<tesseract::BannedMember> members);

    // Disable one row's Unban button while its request is in flight (or
    // re-enable it after a failure). No-op for an unknown user_id.
    void set_unban_pending(const std::string& user_id, bool pending);

    // Drop one row after its unban succeeded. Used instead of a refetch: the
    // local store only learns of the unban once sync delivers the new
    // membership event, so re-reading it right away would still list the
    // user. No-op for an unknown user_id.
    void remove_banned_member(const std::string& user_id);

    // Fired when a row's Unban button is clicked.
    std::function<void(std::string user_id, std::string display_name)> on_unban_requested;

    // Test accessors.
    const std::vector<tesseract::BannedMember>& banned_members() const { return members_; }
    bool showing_empty_label() const { return empty_label_ != nullptr; }
    bool showing_loading_label() const { return loading_label_ != nullptr; }
    // Clicks row i's Unban button (as the user would). False if no such
    // row or the button is disabled.
    bool click_unban_for_test(int i);

private:
    class BannedRow; // defined in the .cpp
    void rebuild_();

    bool loading_ = false;
    std::vector<tesseract::BannedMember> members_;
    std::vector<std::string> pending_; // user_ids with an unban in flight

    SettingsGroup*         group_         = nullptr;
    tk::Label*             loading_label_ = nullptr;
    tk::Label*             empty_label_   = nullptr;
    std::vector<BannedRow*> rows_; // borrowed; owned by group_
};

} // namespace tesseract::views
