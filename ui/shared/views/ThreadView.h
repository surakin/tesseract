#pragma once

// Right-panel widget showing one thread: an embedded MessageListView with a
// floating "×" close button overlaid in the top-right corner. There is no
// header strip — the message list fills the entire panel and the close
// button paints on top of it.
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
#include "tk/widget.h"
#include "views/MessageListView.h"

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

    MessageListView* message_list() { return message_list_; }

    // Fires when the user clicks the floating close button.
    std::function<void()> on_close;

    // tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    // Layout constants — exposed for tests so they can place pointer events
    // on the close button without re-computing the layout themselves.
    // An empty header strip sits above the message list to give the close
    // button its own space rather than floating over the first row.
    static constexpr float kHeaderH    = 48.0f;
    static constexpr float kCloseSz    = 32.0f;
    static constexpr float kCloseInset = 8.0f;

private:
    // Borrowed pointers — ownership is in the tk::Widget child list.
    MessageListView* message_list_ = nullptr;
    tk::Button*      close_btn_    = nullptr;
};

} // namespace tesseract::views
