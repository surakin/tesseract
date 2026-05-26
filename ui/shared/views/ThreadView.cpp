#include "ThreadView.h"
#include "media_utils.h"

#include "tk/theme.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace tesseract::views
{

namespace
{

// UTF-8-safe truncation: clip to `max_bytes` bytes, then back off until the
// next byte is a UTF-8 start byte so we never split a code-point. Appends
// "..." when truncation actually trimmed text. Newlines are folded to single
// spaces because the header preview is single-line.
std::string truncate_utf8(std::string s, std::size_t max_bytes)
{
    for (char& c : s)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }
    if (s.size() <= max_bytes)
    {
        return s;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    s.resize(cut);
    s += "...";
    return s;
}

} // namespace

ThreadView::ThreadView()
{
    auto msg = std::make_unique<MessageListView>();
    message_list_ = add_child(std::move(msg));

    auto compose = std::make_unique<ComposeBar>();
    compose_bar_ = add_child(std::move(compose));

    // ComposeBar fires `on_send` with the plain body for non-attachment,
    // non-reply, non-edit sends. ThreadView forwards through `on_send`
    // (with an empty `formatted_body` for now — markdown rendering is a
    // separate concern; the signature leaves room for it).
    compose_bar_->on_send =
        [this](const std::string& body)
    {
        if (on_send)
        {
            on_send(body, std::string{});
        }
    };
}

void ThreadView::set_thread(std::string root_event_id,
                            MessageRowData root_preview)
{
    thread_root_  = std::move(root_event_id);
    root_preview_ = std::move(root_preview);
    // Drop any in-flight close press: header content changed.
    press_close_ = false;
}

void ThreadView::set_messages(std::vector<MessageRowData> rows,
                              bool room_switch)
{
    // Strip thread_root_id so MessageListView's defence-in-depth filter
    // does not drop these rows. See file header.
    for (auto& r : rows)
    {
        r.thread_root_id.clear();
    }
    if (message_list_)
    {
        message_list_->set_messages(std::move(rows), room_switch);
    }
}

void ThreadView::insert_message(std::size_t index, MessageRowData row)
{
    row.thread_root_id.clear();
    if (message_list_)
    {
        message_list_->insert_message(index, std::move(row));
    }
}

void ThreadView::update_message(std::size_t index, MessageRowData row)
{
    row.thread_root_id.clear();
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

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ThreadView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever space the parent allots.
    return constraints;
}

void ThreadView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;

    header_rect_ = {bounds.x, bounds.y, bounds.w, kHeaderH};

    // Close button anchored to the right edge of the header, vertically
    // centred.
    const float close_x = bounds.x + bounds.w - kCloseSz - kPadX;
    const float close_y = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
    close_rect_ = {close_x, close_y, kCloseSz, kCloseSz};

    const float compose_h =
        compose_bar_ ? compose_bar_->natural_height() : ComposeBar::kMinHeight;

    const float list_top = bounds.y + kHeaderH;
    const float list_bot = bounds.y + bounds.h - compose_h;
    const float list_h   = std::max(0.0f, list_bot - list_top);

    if (message_list_)
    {
        message_list_->arrange(lc, {bounds.x, list_top, bounds.w, list_h});
    }

    if (compose_bar_)
    {
        compose_bar_->arrange(
            lc, {bounds.x, list_bot, bounds.w, compose_h});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void ThreadView::paint(tk::PaintCtx& ctx)
{
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Panel background.
    cv.fill_rect(bounds_, pal.bg);

    // Header strip with preview + close button.
    cv.fill_rect(header_rect_, pal.chrome_bg);

    // Bottom border separating header from the list.
    cv.fill_rect({header_rect_.x, header_rect_.bottom() - 1.0f,
                  header_rect_.w, 1.0f},
                 pal.separator);

    // "Thread" label (small, secondary) above the root preview.
    const float text_left  = header_rect_.x + kPadX;
    const float text_right = close_rect_.x - kPadX;
    const float text_max_w = std::max(0.0f, text_right - text_left);

    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        auto label = ctx.factory.build_text("Thread", st);
        if (label)
        {
            cv.draw_text(*label,
                         {text_left, header_rect_.y + 6.0f},
                         pal.text_secondary);
        }
    }

    // Root preview: "<sender_name>: <body snippet>".
    {
        std::string preview;
        if (!root_preview_.sender_name.empty())
        {
            preview = root_preview_.sender_name + ": ";
        }
        preview += truncate_utf8(root_preview_.body, 80);

        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        auto layout = ctx.factory.build_text(preview, st);
        if (layout)
        {
            cv.draw_text(*layout,
                         {text_left, header_rect_.y + 22.0f},
                         pal.text_primary);
        }
    }

    // Close-button background tint when pressed.
    if (press_close_)
    {
        cv.fill_rounded_rect(close_rect_, 4.0f, pal.subtle_pressed);
    }

    // Close glyph: centered "×" (multiplication sign, U+00D7).
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        auto x_layout = ctx.factory.build_text("\xC3\x97", st);
        if (x_layout)
        {
            const tk::Size sz = x_layout->measure();
            const float gx = close_rect_.x + (close_rect_.w - sz.w) * 0.5f;
            const float gy = close_rect_.y + (close_rect_.h - sz.h) * 0.5f;
            cv.draw_text(*x_layout, {gx, gy}, pal.text_secondary);
        }
    }

    // Paint embedded children (message list + compose bar).
    for (auto& ch : children())
    {
        if (ch->visible())
        {
            ch->paint(ctx);
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ThreadView::on_pointer_down(tk::Point local)
{
    // `dispatch_pointer_down` already walked into children first; we only see
    // points that none of them claimed. The close button lives in the header
    // strip outside both child rects, so it's the only target here.
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (rect_contains(close_rect_, w))
    {
        press_close_ = true;
        return true;
    }
    return false;
}

void ThreadView::on_pointer_up(tk::Point local, bool /*inside_self*/)
{
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (press_close_)
    {
        press_close_ = false;
        if (rect_contains(close_rect_, w))
        {
            if (on_close)
            {
                on_close();
            }
        }
    }
}

} // namespace tesseract::views
