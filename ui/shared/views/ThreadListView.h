#pragma once

// Right-side panel listing every known thread in the current room. Built
// from Client::list_room_threads(room_id) and wired up by RoomView. Each
// row shows the thread root preview, the latest reply preview, and the
// reply count. Clicking a row opens the thread; clicking the floating "×"
// button in the top-right corner hides the panel. There is no header
// strip — the list fills the entire panel and the close button paints on
// top.

#include "tk/canvas.h"
#include "tk/controls.h"
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
    // bottom of the list — RoomView uses this to call
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
    // An empty header strip sits above the rows to give the close button
    // its own space rather than floating over the first thread row.
    static constexpr float kHeaderH    = 48.0f;
    static constexpr float kRowH       = 64.0f;
    static constexpr float kCloseSz    = 32.0f;
    static constexpr float kCloseInset = 8.0f;
    static constexpr float kPadX       = 12.0f;
    static constexpr float kPadY       = 8.0f;

private:
    std::vector<tesseract::ThreadInfo> threads_;

    // Floating close button — added as a child so it claims pointer downs
    // before they reach the row hit-test below.
    tk::Button* close_btn_ = nullptr;

    // Layout rects (world-space, refreshed on arrange).
    std::vector<tk::Rect> row_rects_;

    // Press-state for the click-then-release pattern on rows. -1 = none.
    int press_row_ = -1;
};

} // namespace tesseract::views
