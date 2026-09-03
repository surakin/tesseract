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
#include "tk/svg.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ThreadListView : public tk::ListView, public tk::ListAdapter,
                       public tk::ListAdapterAccessibility
{
public:
    ThreadListView();
    ~ThreadListView() override = default;

    void set_threads(std::vector<tesseract::ThreadInfo> threads);
    const std::vector<tesseract::ThreadInfo>& threads() const { return threads_; }

    // Filters visible rows to threads whose root/latest preview text matches
    // `q` (case-insensitive substring, empty = show all). Wired to the
    // header's search field.
    void set_search_text(std::string q);
    const std::string& search_text() const { return search_text_; }

    // Shell callbacks.
    std::function<void()> on_close;
    std::function<void(const std::string& root_event_id)> on_thread_clicked;
    // Fired by the header "mark all threads as read" button (list-checks
    // icon). Only enabled while at least one thread is unread.
    std::function<void()> on_mark_all_read;
    // on_near_top is inherited from tk::ListView — ShellBase wires it
    // to call paginate_room_threads() for older threads (loaded above).

    // tk::ListView / tk::Widget overrides.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // Overrides paint() directly (rather than paint_before_children()) since
    // the base tk::ListView already overrides paint() itself — see the .cpp
    // for why that makes paint_before_children() unreachable here.
    void     paint(tk::PaintCtx&) override;
    void     on_theme_changed(const tk::Theme&) override;

    // tk::ListAdapter overrides.
    std::size_t count() const override;
    float       measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                                   float available_width) override;
    void        paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                          bool selected, bool hovered) override;
    bool        is_selectable(std::size_t index) const override;

    // tk::ListAdapterAccessibility overrides. Mirrors paint_row's own
    // index-0-is-header-spacer convention.
    tk::Role access_role_for_row(std::size_t index) const override;
    std::string access_name_for_row(std::size_t index) const override;
    // Mirrors on_row_clicked's identical index-1-based dispatch, so the
    // mouse and AT-client activation paths can't drift apart.
    bool access_activate_row(std::size_t index) override;

    // Layout constants — exposed for tests.
    // A header strip sits above the scrollable rows to house the search
    // field and close button.
    static constexpr float kHeaderH    = 48.0f;
    static constexpr float kRowH       = 64.0f;
    static constexpr float kCloseSz    = 32.0f;
    static constexpr float kCloseInset = 8.0f;
    static constexpr float kPadX       = 12.0f;
    static constexpr float kPadY       = 8.0f;
    static constexpr float kSearchGap  = 8.0f;
    static constexpr float kSearchInsetY = 8.0f;
    // Gap between the two header icon buttons (mark-all / close).
    static constexpr float kHeaderBtnGap = 4.0f;

private:
    void rebuild_filtered_();
    // Enable/disable + tint the "mark all read" button from the current
    // threads_ (any unread → enabled + active tint, else disabled + muted).
    void refresh_mark_all_enabled_();

    std::vector<tesseract::ThreadInfo> threads_;
    std::string search_text_;
    // Indices into threads_ that pass the current search filter, in display
    // order (row index 1 == filtered_indices_[0], etc.). Rebuilt whenever
    // threads_ or search_text_ changes.
    std::vector<std::size_t> filtered_indices_;

    // Floating close button in the header strip — added as a child so
    // pointer dispatch reaches it before the ListView row hit-test.
    tk::Button* close_btn_ = nullptr;
    // "Mark all threads as read" button, immediately left of close_btn_.
    tk::Button* mark_all_btn_ = nullptr;
    // Cached from on_theme_changed so refresh_mark_all_enabled_() can pick a
    // tint without a Theme& in hand.
    tk::Color mark_all_tint_active_{};
    tk::Color mark_all_tint_muted_{};
    // Header search field — null when constructed without a Host (tests).
    tk::TextField* search_field_ = nullptr;
};

} // namespace tesseract::views
