#pragma once

// Right-side panel listing every known thread in the current room. Built
// from Client::list_room_threads(room_id) and wired up by RoomView (Task 8).
// Each row shows the thread root preview, the latest reply preview, and
// the reply count. Clicking a row opens the thread; clicking the close
// button hides the panel.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadListView : public tk::Widget
{
public:
    ThreadListView();
    ~ThreadListView() override = default;

    void set_threads(std::vector<tesseract::ThreadInfo> threads);
    const std::vector<tesseract::ThreadInfo>& threads() const { return threads_; }

    // Shell callbacks.
    std::function<void()> on_close;
    std::function<void(const std::string& root_event_id)> on_thread_clicked;
    // Fires when the user scrolls (or the layout settles) close to the
    // bottom of the list — RoomView (Task 8) uses this to call
    // paginate_room_threads() for older threads.
    std::function<void()> on_near_bottom;

    // tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // Layout constants — exposed for tests so they can place pointer
    // events on a row without re-computing the layout themselves.
    static constexpr float kHeaderH = 36.0f;
    static constexpr float kRowH    = 64.0f;
    static constexpr float kCloseSz = 24.0f;
    static constexpr float kPadX    = 12.0f;
    static constexpr float kPadY    = 8.0f;

private:
    std::vector<tesseract::ThreadInfo> threads_;

    // Layout rects (world-space, refreshed on arrange).
    tk::Rect header_rect_{};
    tk::Rect close_rect_{};
    std::vector<tk::Rect> row_rects_;

    // Press-state for the click-then-release pattern. -1 = nothing pressed.
    int  press_row_  = -1;
    bool press_close_ = false;
};

} // namespace tesseract::views
