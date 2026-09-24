#pragma once

// Invite-to-room dialog — a centred modal card owned by RoomView (so pop-out
// room windows get it too), opened from RoomInfoPanel's "Invite people"
// button. Modelled on ForwardRoomPicker: dim backdrop, a search field at the
// top, a scrollable multi-select user list, and a Cancel / Invite (N) footer
// that switches to a progress / error body while the invites are in flight.
//
// The search field is a pill-capable tk::TextArea, and its mention pills ARE
// the selection: checking a row inserts that user's pill (replacing the
// typed filter text), unchecking removes it, and deleting a pill with
// Backspace unchecks its row. Text typed after the pills filters the known-
// users roster; a complete "@user:server" that is followed by a separator
// (space / comma / Enter, or anything pasted before more content) becomes a
// pill once it is known — immediately for a roster hit, otherwise after the
// shell resolves its profile (set_resolved_user). An mxid whose profile
// doesn't exist stays as text and is listed as "User not found". See
// InviteFieldModel for the parsing rules.

#include "InviteFieldModel.h"

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/text_area.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{

class InviteDialog : public tk::Widget
{
protected:
    // host() is nullable: when null (unit tests constructing the dialog
    // detached) the search field is skipped — search_field() stays null.
    InviteDialog();
    TK_WIDGET_FACTORY_FRIEND(InviteDialog)

public:
    struct UserEntry
    {
        std::string user_id;
        std::string display_name;
        std::string avatar_url;
    };

    // Filter the known-users roster by `needle` (case-insensitive substring
    // on name + mxid; empty = all), already sorted and capped for display.
    using UsersFilter = std::function<std::vector<UserEntry>(const std::string& needle)>;
    // Exact roster lookup by mxid.
    using UserLookup = std::function<std::optional<UserEntry>(const std::string& user_id)>;
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    ~InviteDialog() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open(const std::string& room_id, const std::string& room_name);
    void close();
    bool is_open() const { return is_open_; }
    const std::string& room_id() const { return room_id_; }

    // ── Data ──────────────────────────────────────────────────────────────
    void set_users_filter(UsersFilter f);
    void set_user_lookup(UserLookup f);
    void set_avatar_provider(AvatarProvider p);
    // Users already in the room (joined/invited) — listed but not selectable.
    void set_existing_members(const std::vector<std::string>& user_ids);
    // The shell's roster changed (build progressed / a profile resolved) —
    // re-filter and re-try converting committed mxids into pills.
    void refresh_candidates();
    // Outcome of an on_resolve_user request. nullopt = no such user.
    void set_resolved_user(const std::string& user_id,
                           std::optional<UserEntry> entry);
    // Re-render every pill's avatar from the avatar provider (call when a
    // user avatar finished decoding).
    void refresh_pill_avatars();

    // The self-owned search field, or null when constructed without a Host.
    tk::TextArea* search_field() const { return search_field_; }

    // ── Keyboard ──────────────────────────────────────────────────────────
    void move_highlight(int delta);
    // Toggle the highlighted row's selection (Enter with a non-empty query).
    void toggle_highlighted();
    // Fire on_invite_confirmed with the current selection.
    void confirm();

    // ── Async invite state ────────────────────────────────────────────────
    // Call set_inviting() after on_invite_confirmed; the dialog stays open
    // showing progress. add_invite_error() per failure, mark_complete()
    // once every invite resolved (closes when there were no errors).
    void set_inviting(int user_count);
    void add_invite_error(const std::string& user, const std::string& detail);
    void mark_complete();
    bool is_inviting() const { return inviting_; }

    // Invite list: the pills, plus the query token when it is a complete,
    // known mxid (so "type an mxid, click Invite" works without Enter).
    std::vector<std::string> selected_user_ids() const;

    // ── Callbacks ─────────────────────────────────────────────────────────
    std::function<void(std::vector<std::string> user_ids)> on_invite_confirmed;
    // Look up an mxid that isn't in the roster. `debounce` is true for the
    // token still being typed (the shell coalesces keystrokes), false for a
    // committed one. Answer with set_resolved_user().
    std::function<void(const std::string& user_id, bool debounce)> on_resolve_user;
    // A row/pill avatar isn't cached yet.
    std::function<void(const std::string& user_id, const std::string& mxc)>
        on_user_avatar_needed;
    std::function<void()> on_close;
    // Card geometry changed (field grew/shrank, row count changed).
    std::function<void()> on_layout_changed;

    void on_theme_changed(const tk::Theme& t) override;
    // Shadow (Widget::set_visible is non-virtual) that also hides the
    // native field — same idiom as ForwardRoomPicker.
    void set_visible(bool v);

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

    static constexpr float kCardW     = 560.0f;
    static constexpr float kCardMaxH  = 560.0f;
    static constexpr float kTitleH    = 44.0f;
    static constexpr float kFooterH   = 52.0f;
    static constexpr float kRowH      = 48.0f;
    static constexpr float kFieldMinH = 34.0f;
    static constexpr float kFieldMaxH = 96.0f;

    enum class RowState
    {
        Selected,      // pill in the field
        Available,     // checkable candidate
        AlreadyMember, // in the room already — disabled
        Resolving,     // mxid lookup in flight — disabled
        NotFound,      // mxid lookup came back empty — disabled
    };
    struct Row
    {
        UserEntry user;
        RowState  state = RowState::Available;
    };
    // Exposed for tests.
    const std::vector<Row>& rows() const { return rows_; }
    const InviteFieldParse& parse() const { return parse_; }

private:
    class Adapter;
    friend class Adapter;

    // Re-parse the field, turn committed known mxids into pills (repeating
    // until stable), request lookups for unknown ones, then rebuild rows_.
    void reconcile_();
    void rebuild_rows_();
    // Known entry for an mxid: roster first, then resolved_.
    std::optional<UserEntry> known_entry_(const std::string& user_id) const;
    void insert_pill_(int start, int end, const UserEntry& e);
    void remove_pill_(const std::string& user_id);
    void toggle_row_(std::size_t index);
    const tk::Image* avatar_for_(const UserEntry& e);
    float field_height_() const;
    void relayout_();

    bool is_open_       = false;
    bool pending_focus_ = false;
    bool reconciling_   = false;
    std::string room_id_;
    std::string room_name_;

    InviteFieldParse parse_;
    std::vector<Row> rows_;
    // mxid → profile (nullopt = confirmed not found).
    std::unordered_map<std::string, std::optional<UserEntry>> resolved_;
    // mxid → display entry of every pill inserted by this dialog (for
    // avatar refreshes and row rendering of selected users).
    std::unordered_map<std::string, UserEntry> pill_entries_;
    std::unordered_set<std::string> existing_members_;

    UsersFilter    users_filter_;
    UserLookup     user_lookup_;
    AvatarProvider avatar_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView*  list_         = nullptr;
    tk::TextArea*  search_field_ = nullptr;
    float          field_natural_h_ = kFieldMinH;

    tk::Rect card_rect_{};
    tk::Rect field_rect_{};
    tk::Rect cancel_btn_rect_{};
    tk::Rect confirm_btn_rect_{};
    tk::Rect dismiss_btn_rect_{};

    bool press_outside_ = false;
    bool press_cancel_  = false;
    bool press_confirm_ = false;
    bool press_dismiss_ = false;

    bool                     inviting_       = false;
    int                      invite_errors_  = 0;
    std::string              inviting_status_;
    std::vector<std::string> error_lines_;
};

} // namespace tesseract::views
