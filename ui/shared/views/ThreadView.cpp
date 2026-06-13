#include "ThreadView.h"

#include "tk/theme.h"

#include <memory>
#include <utility>

namespace tesseract::views
{

ThreadView::ThreadView()
{
    auto msg = std::make_unique<MessageListView>();
    msg->set_thread_button_visible(false);
    message_list_ = add_child(std::move(msg));

    // Added after message_list_ so it sits topmost in the child list:
    // dispatch_pointer_down walks children in reverse, so the close button
    // claims clicks on its bounds before they can reach the message list,
    // and paint() walks forward so the button paints over the messages.
    auto close = std::make_unique<tk::Button>(
        "\xC3\x97", // U+00D7 ×
        std::function<void()>{}, tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] {
        if (on_close) on_close();
    });
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
    if (message_list_)
    {
        // Message list sits below the empty header strip.
        const float list_top = bounds.y + kHeaderH;
        const float list_h   = std::max(0.0f, bounds.bottom() - list_top);
        message_list_->arrange(lc, {bounds.x, list_top, bounds.w, list_h});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void ThreadView::paint(tk::PaintCtx& ctx)
{
    // Panel background — painted under the message list so empty / small
    // thread lists don't reveal the parent surface behind them.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Children: message_list_ first (so it paints under), close_btn_ last
    // (so the floating button reads above the rows).
    for (auto& ch : children())
    {
        if (ch->visible())
        {
            ch->paint(ctx);
        }
    }

    // tk::Button(Icon) only paints its hover/press background — the glyph
    // is expected to be drawn by the parent (mirroring RoomInfoPanel /
    // ComposeBar). Draw the "×" centred inside the button so it stays
    // visible at rest.
    if (close_btn_ && close_btn_->visible())
    {
        const tk::Rect cb = close_btn_->bounds();
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        auto glyph = ctx.factory.build_text("\xC3\x97", st); // U+00D7 ×
        if (glyph)
        {
            const tk::Size sz = glyph->measure();
            const float gx = cb.x + (cb.w - sz.w) * 0.5f;
            const float gy = cb.y + (cb.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*glyph, {gx, gy},
                                 ctx.theme.palette.text_secondary);
        }
    }
}

} // namespace tesseract::views
