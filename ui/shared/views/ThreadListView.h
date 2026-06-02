#pragma once

// Right-side panel listing every known thread in the current room. Built
// from Client::list_room_threads(room_id) and wired up by RoomView. Each
// row shows the thread root preview, the latest reply preview, and the
// reply count. Clicking a row opens the thread; clicking the floating "×"
// button in the header strip hides the panel.
//
// Inherits tk::ListView (scrolling + scrollbar + on_near_top) and
// implements tk::ListAdapter (one fixed-height row per ThreadInfo).
// Threads sort newest-last (newest at the bottom), matching the message
// timeline; older threads page in when scrolling up toward the top.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadListView : public tk::ListView, public tk::ListAdapter
{
public:
    ThreadListView();
    ~ThreadListView() override = default;

    void set_threads(std::vector<tesseract::ThreadInfo> threads);
    const std::vector<tesseract::ThreadInfo>& threads() const { return threads_; }

    // Shell callbacks.
    std::function<void()> on_close;
    std::function<void(const std::string& root_event_id)> on_thread_clicked;
    // on_near_top is inherited from tk::ListView — ShellBase wires it
    // to call paginate_room_threads() for older threads (loaded above).

    // tk::ListView / tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    // tk::ListAdapter overrides.
    std::size_t count() const override;
    float       measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                                   float available_width) override;
    void        paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                          bool selected, bool hovered) override;
    bool        is_selectable(std::size_t index) const override;

    // Layout constants — exposed for tests.
    // A header strip sits above the scrollable rows to house the close button.
    static constexpr float kHeaderH    = 48.0f;
    static constexpr float kRowH       = 64.0f;
    static constexpr float kCloseSz    = 32.0f;
    static constexpr float kCloseInset = 8.0f;
    static constexpr float kPadX       = 12.0f;
    static constexpr float kPadY       = 8.0f;

private:
    std::vector<tesseract::ThreadInfo> threads_;

    // Floating close button in the header strip — added as a child so
    // pointer dispatch reaches it before the ListView row hit-test.
    tk::Button* close_btn_ = nullptr;
};

} // namespace tesseract::views
