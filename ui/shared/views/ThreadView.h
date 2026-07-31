#pragma once

// Right-panel widget showing one thread: a header row — an embedded
// RoomSearchBar (find-in-thread, always visible, with its own close/paginate
// chrome hidden) plus a floating "×" close button for the whole panel — above
// an embedded MessageListView.
//
// Sends are handled by the main room ComposeBar; when the thread panel is
// open RoomView routes plain sends through on_thread_send → on_thread_send_requested.
//
// Note on the thread_root_id strip: MessageListView::{set,insert,update}_message
// drop rows whose `thread_root_id` is non-empty as a defence-in-depth filter
// against in-thread replies leaking into the main timeline. ThreadView is the
// place those replies *do* belong, so we clear that field on every row we
// forward to the embedded list. The thread context is implicit from this
// widget, so dropping the field at the boundary is safe.

#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/widget.h"
#include "views/MessageListView.h"
#include "views/RoomSearchBar.h"

#include <functional>
#include <vector>

namespace tesseract::views
{

class ThreadView : public tk::Widget
{
public:
    ThreadView();
    ~ThreadView() override = default;

    // Delegate to the embedded MessageListView, after stripping
    // `thread_root_id` so MessageListView's defence-in-depth filter does not
    // drop the reply rows (see the file header).
    void set_messages(std::vector<MessageRowData> rows, bool room_switch);
    void insert_message(std::size_t index, MessageRowData row);
    void update_message(std::size_t index, MessageRowData row);
    void remove_message(std::size_t index);
    void append_messages(std::vector<MessageRowData> rows);
    void prepend_messages(std::vector<MessageRowData> rows);

    MessageListView* message_list() { return message_list_; }

    // Find-in-thread search bar — embedded directly in the header, visible
    // from construction (no toggle). The shell (ShellBase) wires its
    // on_query_changed / on_navigate callbacks and drives set_match_status()
    // the same way RoomView does for its own room_search_bar().
    RoomSearchBar* search_bar() const { return search_bar_; }
    // Resets the query (and, via the shell's on_query_changed handling,
    // matches/highlights) without touching focus — for switching to a
    // different thread underneath the always-open bar.
    void reset_search();

    // Fires when the user clicks the floating close button.
    std::function<void()> on_close;

    // tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;
    void     paint_after_children(tk::PaintCtx&) override;

    // Layout constants — exposed for tests so they can place pointer events
    // on the close button without re-computing the layout themselves.
    // The header row is the embedded RoomSearchBar itself, so it must be
    // exactly RoomSearchBar::kStripH tall — RoomSearchBar centers its own
    // buttons using that constant directly rather than its arranged bounds'
    // height.
    static constexpr float kHeaderH    = RoomSearchBar::kStripH;
    static constexpr float kCloseSz    = 32.0f;
    static constexpr float kCloseInset = 8.0f;
    static constexpr float kHeaderGap  = 4.0f;

private:
    // Borrowed pointers — ownership is in the tk::Widget child list.
    MessageListView* message_list_ = nullptr;
    tk::Button*      close_btn_    = nullptr;
    RoomSearchBar*   search_bar_   = nullptr;
    // Cached Lucide close icon (kCloseSvg), tint-aware — see paint().
    tk::IconCache close_icon_;
};

} // namespace tesseract::views
