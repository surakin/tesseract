#include "ThreadView.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace tesseract::views
{

ThreadView::ThreadView()
{
    auto msg = std::make_unique<MessageListView>();
    msg->set_thread_button_visible(false);
    message_list_ = add_child(std::move(msg));

    // Find-in-thread search bar — embedded directly as the header, visible
    // from construction. No close/paginate chrome of its own: the panel's
    // own "×" (close_btn_ below) is the sole close affordance, and a
    // thread's messages are already fully loaded (via subscribe_thread), so
    // there's nothing to paginate through while searching.
    auto bar = tk::create_widget<RoomSearchBar>(this);
    search_bar_ = add_child(std::move(bar));
    search_bar_->set_show_close_button(false);
    search_bar_->set_show_paginate(false);
    search_bar_->set_visible(true);
    search_bar_->open();

    // Added after message_list_/search_bar_ so it sits topmost in the child
    // list: dispatch_pointer_down walks children in reverse, so the close
    // button claims clicks on its bounds before they reach the search bar or
    // message list, and paint() walks forward so it paints over them.
    // The label_ passed below is only internal scaffolding (mirrors
    // ComposeBar's identical convention) — the actual glyph is a Lucide
    // kCloseSvg icon drawn in paint(), and the accessible name is set
    // explicitly since the raw × glyph isn't one.
    auto close = tk::create_widget<tk::Button>(this,
        "\xC3\x97", // U+00D7 ×
        std::function<void()>{}, tk::Button::Variant::Icon);
    close->set_accessible_name(tk::tr("Close"));
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] {
        if (on_close) on_close();
    });
}

void ThreadView::reset_search()
{
    if (search_bar_)
        search_bar_->clear_query();
}

namespace
{

void strip_thread_fields(MessageRowData& r)
{
    r.thread_root_id.clear();
    r.is_thread_root              = false;
    r.thread_reply_count          = 0;
    r.thread_latest_sender_name.clear();
    r.thread_latest_body.clear();
    r.thread_latest_ts            = 0;
}

} // namespace

void ThreadView::set_messages(std::vector<MessageRowData> rows,
                              bool room_switch)
{
    for (auto& r : rows)
    {
        strip_thread_fields(r);
    }
    if (message_list_)
    {
        message_list_->set_messages(std::move(rows), room_switch);
    }
}

void ThreadView::insert_message(std::size_t index, MessageRowData row)
{
    strip_thread_fields(row);
    if (message_list_)
    {
        message_list_->insert_message(index, std::move(row));
    }
}

void ThreadView::update_message(std::size_t index, MessageRowData row)
{
    strip_thread_fields(row);
    if (message_list_)
    {
        message_list_->update_message(index, std::move(row));
    }
}

void ThreadView::remove_message(std::size_t index)
{
    if (message_list_)
    {
        message_list_->remove_message(index);
    }
}

void ThreadView::append_messages(std::vector<MessageRowData> rows)
{
    for (auto& r : rows)
        strip_thread_fields(r);
    if (message_list_)
        message_list_->append_messages(std::move(rows));
}

void ThreadView::prepend_messages(std::vector<MessageRowData> rows)
{
    for (auto& r : rows)
        strip_thread_fields(r);
    if (message_list_)
        message_list_->prepend_messages(std::move(rows));
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ThreadView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever space the parent allots.
    return constraints;
}

void ThreadView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    if (close_btn_)
    {
        const float cx = bounds.x + bounds.w - kCloseSz - kCloseInset;
        const float cy = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
        close_btn_->arrange(lc, {cx, cy, kCloseSz, kCloseSz});
    }
    if (search_bar_)
    {
        // The find bar IS the header — it fills the header row up to (but
        // not overlapping) the panel's own close button. Its own close/
        // paginate chrome is hidden (see the constructor), so its
        // right-aligned content (count/up-down) already flushes right
        // against this narrowed width instead of leaving a gap.
        const float bar_w = std::max(
            0.0f, bounds.w - kCloseSz - kCloseInset - kHeaderGap);
        search_bar_->arrange(lc, {bounds.x, bounds.y, bar_w, kHeaderH});
    }
    if (message_list_)
    {
        // Message list sits below the header row.
        const float list_top = bounds.y + kHeaderH;
        const float list_h   = std::max(0.0f, bounds.bottom() - list_top);
        message_list_->arrange(lc, {bounds.x, list_top, bounds.w, list_h});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void ThreadView::paint_before_children(tk::PaintCtx& ctx)
{
    // Panel background — painted under the message list so empty / small
    // thread lists don't reveal the parent surface behind them.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Header band spans the full row (search bar + close button) with one
    // background fill and one separator line. RoomSearchBar paints its own
    // chrome_bg/separator too, but only across its own width (narrowed to
    // leave room for close_btn_ — see arrange()); without this, the close
    // button would sit on the plain panel bg with no separator under it,
    // reading as a disconnected control instead of part of the same header.
    const tk::Rect header{bounds_.x, bounds_.y, bounds_.w, kHeaderH};
    ctx.canvas.fill_rect(header, ctx.theme.palette.chrome_bg);
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y + kHeaderH - 1.0f, bounds_.w, 1.0f},
        ctx.theme.palette.separator);
}

void ThreadView::paint_after_children(tk::PaintCtx& ctx)
{
    // tk::Button(Icon) only paints its hover/press background — the icon
    // is expected to be drawn by the parent (mirroring RoomInfoPanel /
    // ComposeBar). Draw the Lucide close icon centred inside the button.
    if (close_btn_ && close_btn_->visible())
    {
        constexpr float kCloseIconPx = 18.0f;
        const tk::Color tint = close_btn_->hovered()
                                  ? ctx.theme.palette.text_primary
                                  : ctx.theme.palette.text_secondary;
        close_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg, close_btn_->bounds(),
                         kCloseIconPx, tint);
    }
}

} // namespace tesseract::views
