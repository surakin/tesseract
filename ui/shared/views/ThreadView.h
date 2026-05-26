#pragma once

// Right-panel widget showing one thread:
//   - header (root sender + body preview + close "×" button)
//   - embedded MessageListView showing thread replies
//   - embedded ComposeBar at the bottom whose sends are forwarded
//     through `on_send` (the shell wires this to
//     `Client::send_thread_message` in Task 8).
//
// Note on the thread_root_id strip: MessageListView::{set,insert,update}_message
// drop rows whose `thread_root_id` is non-empty as a defence-in-depth filter
// against in-thread replies leaking into the main timeline. ThreadView is the
// place those replies *do* belong, so we clear that field on every row we
// forward to the embedded list. The thread context is implicit from this
// widget, so dropping the field at the boundary is safe.

#include "tk/canvas.h"
#include "tk/widget.h"
#include "views/ComposeBar.h"
#include "views/MessageListView.h"

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadView : public tk::Widget
{
public:
    ThreadView();
    ~ThreadView() override = default;

    // Identify the thread + populate the header preview.
    void set_thread(std::string root_event_id, MessageRowData root_preview);
    const std::string& thread_root() const { return thread_root_; }
    const MessageRowData& root_preview() const { return root_preview_; }

    // Delegate to the embedded MessageListView, after stripping
    // `thread_root_id` so MessageListView's defence-in-depth filter does not
    // drop the reply rows (see the file header).
    void set_messages(std::vector<MessageRowData> rows, bool room_switch);
    void insert_message(std::size_t index, MessageRowData row);
    void update_message(std::size_t index, MessageRowData row);
    void remove_message(std::size_t index);

    MessageListView* message_list() { return message_list_; }
    ComposeBar* compose_bar() { return compose_bar_; }

    // Fires when the user clicks the header close button.
    std::function<void()> on_close;
    // Fires when ComposeBar reports a plain send. `formatted_body` is the
    // HTML-formatted body (currently always empty — wired through for
    // future use). Shell forwards to `Client::send_thread_message`.
    std::function<void(const std::string& body,
                       const std::string& formatted_body)>
        on_send;

    // tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // Layout constants — exposed for tests so they can place pointer events
    // on the close button without re-computing the layout themselves.
    static constexpr float kHeaderH = 48.0f;
    static constexpr float kCloseSz = 24.0f;
    static constexpr float kPadX    = 12.0f;

private:
    std::string thread_root_;
    MessageRowData root_preview_;

    // Borrowed pointers — ownership is in the tk::Widget child list.
    MessageListView* message_list_ = nullptr;
    ComposeBar*      compose_bar_  = nullptr;

    tk::Rect header_rect_{};
    tk::Rect close_rect_{};
    bool press_close_ = false;
};

} // namespace tesseract::views
