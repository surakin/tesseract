#pragma once

// RoomDirectoryView — content for the "Browse" tab of AddRoomView (see
// AddRoomView.h): a searchable, paginated list of the public room
// directory (own homeserver by default, or another server via federation),
// with a Join button for the selected row.
//
// Workflow:
//   1. open() issues an initial search (empty filter, own homeserver).
//   2. User edits the server/search fields. Every keystroke fires
//      on_field_edited (the host debounces this into search_now()); losing
//      focus or pressing Enter or clicking "Search" triggers immediately
//      instead of waiting for the debounce. Either way, search_now() reads
//      the fields' current text and fires on_search_requested with a fresh
//      request_id; the host calls Client::search_room_directory() on a
//      worker and later delivers the first page via set_results() (or an
//      error — e.g. an unparseable server name — via set_search_failed()).
//   3. Scrolling near the list's bottom (while more pages remain) fires
//      on_next_page_requested; the host calls Client::room_directory_next_page()
//      and delivers the appended rows via set_results().
//   4. Clicking a row selects it (highlight only); clicking "Join" fires
//      on_join_requested(room_id).

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class RoomDirectoryView : public tk::Widget
{
protected:
    RoomDirectoryView();
    TK_WIDGET_FACTORY_FRIEND(RoomDirectoryView)

public:
    // Defined out-of-line (RoomDirectoryView.cpp), after Adapter's full
    // definition — unique_ptr<Adapter>'s deleter needs a complete type,
    // and Adapter is only forward-declared here.
    ~RoomDirectoryView() override;

    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    // Issues an initial search if none has run yet. Called by AddRoomView
    // when the Browse tab becomes active/open.
    void open();
    // Cancels any in-flight/stored search and hides. Called by AddRoomView
    // on Cancel/Escape/outside-click/tab-switch-away.
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // Hiding (close()) doesn't cascade to the native field overlays —
    // tk::Widget::set_visible is deliberately non-virtual/non-cascading —
    // so this shadow hides them explicitly, mirroring JoinRoomView's idiom.
    void set_visible(bool v);

    // Suppresses this view's own title row — AddRoomView draws a shared
    // segmented header in its place.
    void set_title_visible(bool v)
    {
        title_visible_ = v;
    }

    void set_avatar_provider(AvatarProvider p);

    // Predicate the view queries to decide the action button's label for
    // the selected row: "Join" for a room the account isn't in yet, "Go"
    // (switches to it, doesn't re-join) for one it already is. Unset means
    // every row is treated as not-yet-joined.
    void set_is_room_joined(std::function<bool(const std::string& room_id)> p);

    // Debounce delay the host should wait after the most recent
    // on_field_edited before calling search_now() — mirrors
    // RoomListView::kSearchDebounceMs's identical rationale.
    static constexpr int kSearchDebounceMs = 400;

    // Starts a fresh search using the fields' current text. Called by the
    // host after its debounce timer elapses; also used internally for
    // Enter/"Search"-click/focus-lost triggers, which bypass the debounce.
    void search_now();

    // Placeholder text for the server field — the logged-in account's own
    // homeserver domain, so users see what "default" means without typing
    // anything. Set once by the host after login.
    void set_homeserver_placeholder(std::string domain);

    // ── Results delivery (called by the host from ShellBase) ──────────────
    // Appends `entries` to the currently displayed list if `request_id`
    // matches the last search/page request issued; stale ids are dropped.
    void set_results(std::uint64_t request_id,
                     std::vector<tesseract::RoomDirectoryEntry> entries,
                     bool reached_end);
    void set_search_failed(std::uint64_t request_id, std::string message);

    // The request_id of the currently active search/page fetch (0 if none
    // has started yet). Used by the host to cancel it when the Browse tab
    // closes.
    std::uint64_t active_request_id() const
    {
        return active_request_id_;
    }

    // ── Join outcome (called by the host once Client::join_room_async
    // resolves) ─────────────────────────────────────────────────────────
    // Re-enables the join button and shows `message` as an error. Success
    // needs no call here — the host closes the whole dialog instead.
    void set_join_failed(std::string message);

    // Fired when the user edits the server or search field (Enter/blur) —
    // `request_id` is freshly allocated by this view for the host to echo
    // back via set_results()/set_search_failed().
    std::function<void(std::uint64_t request_id, const std::string& filter,
                       const std::string& server)>
        on_search_requested;
    // Fired when the list nears its bottom and more pages remain.
    std::function<void(std::uint64_t request_id)> on_next_page_requested;
    // Fired when the user clicks "Join" (or "Go" — see set_is_room_joined).
    // `via_server` is the server the current search was browsing via
    // (empty for the account's own homeserver) — the room was found
    // through it, so it's a reasonable federation route hint for the
    // join itself; the host adds it to Client's `via` list.
    std::function<void(const std::string& room_id, const std::string& via_server)>
        on_join_requested;
    // Fired when a row's avatar isn't in cache yet (lazy fetch, visible
    // rows only — mirrors RoomListView::on_room_avatar_needed).
    std::function<void(const std::string& mxc_url)> on_avatar_needed;
    // Fired on every keystroke in either the server or search field. The
    // host debounces this (kSearchDebounceMs) into a call to search_now().
    std::function<void()> on_field_edited;
    // Fired when the user clicks "Cancel" / "✕".
    std::function<void()> on_cancel;
    // Fired from close() (Cancel, Escape, outside click, or programmatic).
    std::function<void()> on_close;

    // Test accessors.
    bool join_button_enabled() const;
    void trigger_join_for_test();
    std::size_t row_count_for_test() const
    {
        return items_.size();
    }
    void select_row_for_test(int idx)
    {
        select_row_(idx);
    }

    void on_theme_changed(const tk::Theme& t) override;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    class Adapter;

    void request_next_page_();
    void select_row_(int idx);
    // Refreshes join_btn_'s label ("Join" / "Go") for the current
    // selection, per is_room_joined_.
    void update_join_button_label_();

    bool is_open_ = false;
    bool title_visible_ = true;

    std::uint64_t next_request_id_ = 0;
    std::uint64_t active_request_id_ = 0;
    bool has_searched_ = false;
    bool loading_ = false;
    bool reached_end_ = true;
    std::string error_msg_;
    std::string homeserver_placeholder_;
    // Server the current/last search browsed via — passed back on
    // on_join_requested as a federation route hint. Empty = own homeserver.
    std::string active_server_;
    // True from the moment Join/Go is clicked until set_join_failed() runs
    // (success closes the whole dialog instead). Guards against a second
    // click firing a duplicate join while the first is in flight.
    bool joining_ = false;
    // Set when either field's text changes, cleared at the start of every
    // search_now(). Losing focus only re-triggers a search when this is
    // true — otherwise clicking a row (which blurs whichever field was
    // focused) would restart an unchanged search and visibly reset the
    // list back to its top.
    bool fields_dirty_ = false;

    std::vector<tesseract::RoomDirectoryEntry> items_;
    int selected_index_ = -1;

    AvatarProvider avatar_provider_;
    std::function<bool(const std::string& room_id)> is_room_joined_;
    std::unique_ptr<Adapter> adapter_;

    tk::TextField* server_field_ = nullptr;
    tk::TextField* search_field_ = nullptr;
    tk::Button* search_btn_ = nullptr;
    tk::ListView* list_view_ = nullptr;
    tk::Button* join_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Label* status_lbl_ = nullptr;
};

} // namespace tesseract::views
