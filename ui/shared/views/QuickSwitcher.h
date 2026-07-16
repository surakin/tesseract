#pragma once

// Ctrl+K quick switcher — a centred command-palette overlay. Paints a dim
// backdrop over the whole surface with a centred card: a search field at the
// top (a host-overlaid NativeTextField, positioned from search_field_rect())
// and a scrollable room list below. The list shows recent rooms when the query
// is empty and name-filtered rooms as the user types. Enter / click jumps to
// the selected room; Up/Down navigate; Escape (routed by the shell) closes.
//
// Typing a leading '@' switches to "user mode": the shell (via
// on_user_query_changed) filters its known-user roster and live-resolves a
// fully-typed mxid, pushing rows back through set_user_results(). Activating a
// user row fires on_user_selected(mxid) so the shell can open/create a DM.
//
// Mounted as the topmost child of MainAppWidget — set_visible(false) by
// default, arranged at full bounds, painted last (highest z-order). The inner
// tk::ListView is a child so it handles row clicks + wheel scrolling via the
// normal dispatch path; this widget only adds the backdrop / click-outside
// behaviour and keyboard-driven selection.
//
// Avatar bytes live in a host-side cache; pass the same provider lambda the
// rest of the app uses (returns a decoded tk::Image* for an mxc_url, or
// nullptr for the initials-disc fallback).

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class QuickSwitcher : public tk::Widget
{
protected:
    // host() is nullable: when null (e.g. unit tests constructing the
    // switcher directly), the search field is skipped — search_field()
    // stays null and search_field_rect()/set_query() still work for
    // programmatic filtering.
    QuickSwitcher();
    TK_WIDGET_FACTORY_FRIEND(QuickSwitcher)

public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    // Returns the room list to show, already in the order the switcher should
    // display it (the shell sorts most-recent-first). Pulled fresh on open().
    using RoomsProvider = std::function<std::vector<tesseract::RoomInfo>()>;

    // A user row shown in "user mode" (the query starts with '@'). The shell
    // owns sourcing/filtering/ordering and pushes the list via set_user_results.
    struct UserEntry
    {
        std::string user_id;      // "@alice:server"
        std::string display_name; // falls back to the localpart
        std::string avatar_url;   // mxc:// or empty
    };

    // Room mode is the default name-filtered room list; User mode is entered
    // when the query starts with '@' and renders set_user_results() entries.
    enum class Mode
    {
        Room,
        User
    };

    ~QuickSwitcher() override; // out-of-line — Adapter is opaque here

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open();
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // ── Data + query ──────────────────────────────────────────────────────
    void set_rooms_provider(RoomsProvider p);
    // Recent (most-recently-visited) rooms, in visit order — feeds the
    // horizontal "Recent" strip shown when the query is empty.
    void set_recent_provider(RoomsProvider p);
    // Host pipes its NativeTextField's text changes in here. A leading '@'
    // switches the switcher into user mode (see on_user_query_changed).
    void set_query(const std::string& q);
    void set_avatar_provider(AvatarProvider p);
    // Replace the user-mode row list (shell-supplied, already ordered). Applied
    // only while in user mode; ignored once the query no longer starts with '@'.
    void set_user_results(std::vector<UserEntry> users);

    // ── Keyboard navigation (driven by the field's popup-nav callback) ────
    void move_selection(int delta);
    void activate_selected();

    // ── Native-field rect delegation (mirrors RoomListView::search_field_rect)
    tk::Rect search_field_rect() const
    {
        return search_field_rect_;
    }
    bool search_field_visible() const
    {
        return is_open_;
    }

    // The self-owned search field, or null when constructed without a
    // Host. The shell pushes its Up/Down/Escape handling onto it via
    // push_popup_nav() (Tab/Shift-Tab are already claimed internally).
    tk::TextField* search_field() const
    {
        return search_field_;
    }

    void on_theme_changed(const tk::Theme& t) override;

    // Hiding the switcher (close()) doesn't cascade to the search field —
    // tk::Widget::set_visible is deliberately non-virtual/non-cascading —
    // so this shadow hides it explicitly, mirroring QRGrantView's idiom.
    void set_visible(bool v);

    // ── Callbacks ─────────────────────────────────────────────────────────
    // Fires when the overlay should be dismissed (Escape, outside click).
    std::function<void()> on_close;
    // Fires when the user activates a room (Enter / click).
    std::function<void(const std::string& room_id)> on_room_selected;
    // Fires from paint for a visible row whose avatar isn't cached yet.
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;
    // Fires on every query change while in user mode (query starts with '@').
    // The shell filters its known-user roster + live-resolves a typed mxid and
    // pushes results back via set_user_results().
    std::function<void(const std::string& query)> on_user_query_changed;
    // Fires when the user activates a user row (Enter / click) in user mode.
    std::function<void(const std::string& mxid)> on_user_selected;
    // Fires from paint for a visible user row whose avatar isn't cached yet.
    std::function<void(const std::string& mxc_url)> on_user_avatar_needed;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

    static constexpr float kCardW = 560.0f;
    static constexpr float kCardMaxH = 480.0f;
    static constexpr float kHeaderH = 52.0f;
    static constexpr float kRowH = 44.0f;

    // Horizontal "Recent" strip (shown only when the query is empty).
    static constexpr float kRecentStripH = 92.0f; // caption + chip row
    static constexpr float kRecentChipW = 64.0f;
    static constexpr float kRecentChipGap = 8.0f;
    static constexpr float kRecentAvatar = 40.0f;

private:
    class Adapter;
    friend class Adapter;

    // Rebuild filtered_ from all_rooms_ + query_, then reset the inner list's
    // selection to the first row and scroll to the top.
    void refilter_();

    // Number of rows in the active mode (filtered rooms or user results).
    std::size_t active_count_() const;

    // True when the Recent strip should be shown (Room mode + query empty +
    // recent_ non-empty).
    bool show_recent_() const;
    // Paint the horizontal "Recent" strip and (re)populate recent_chips_.
    void paint_recent_strip_(tk::PaintCtx& ctx);

    // Set by open(); consumed by the next paint(). Deferred rather than
    // focusing synchronously inside open() because arrange() — which
    // positions search_field_'s native overlay via set_rect() — hasn't run
    // yet at that point (open() precedes the shell's own relayout() call).
    // Mirrors RoomView::pending_default_focus_'s identical rationale.
    bool pending_focus_ = false;

    bool is_open_ = false;
    Mode mode_ = Mode::Room;
    std::string query_;
    std::vector<tesseract::RoomInfo> all_rooms_;
    std::vector<tesseract::RoomInfo> filtered_;
    std::vector<tesseract::RoomInfo> recent_;
    // Active row list in user mode (set_user_results); empty in room mode.
    std::vector<UserEntry> user_results_;

    RoomsProvider rooms_provider_;
    RoomsProvider recent_provider_;
    AvatarProvider avatar_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_ = nullptr;

    tk::Rect card_rect_{};
    tk::Rect search_field_rect_{};
    tk::TextField* search_field_ = nullptr; // owned via add_child when host provided
    tk::Rect recent_strip_rect_{};
    // Per-chip hit rects (widget-local) + room id, rebuilt each paint.
    std::vector<std::pair<tk::Rect, std::string>> recent_chips_;
    // Index of the chip currently pressed (-1 = none).
    int pressed_chip_ = -1;
    // True while a pointer-down landed on the dim backdrop (outside the card);
    // a pointer-up that also lands outside dismisses the overlay.
    bool press_outside_ = false;
};

} // namespace tesseract::views
