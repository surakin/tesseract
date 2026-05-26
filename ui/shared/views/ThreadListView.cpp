#include "ThreadListView.h"
#include "media_utils.h"

#include "tk/theme.h"

#include <algorithm>
#include <cstdio>

namespace tesseract::views
{

namespace
{

// UTF-8-safe truncation: clip to `max_bytes` bytes, then back off until the
// next byte is a UTF-8 start byte so we never split a code-point. Appends
// "..." when truncation actually trimmed text. Newlines are folded to single
// spaces because the previews are single-line.
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

ThreadListView::ThreadListView() = default;

void ThreadListView::set_threads(std::vector<tesseract::ThreadInfo> threads)
{
    threads_ = std::move(threads);
    // Drop any in-flight press: the row indices it refers to may now
    // point at a different thread (or nothing).
    press_row_   = -1;
    press_close_ = false;
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ThreadListView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // The panel fills whatever space its parent allots.
    return constraints;
}

void ThreadListView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    header_rect_ = {bounds.x, bounds.y, bounds.w, kHeaderH};

    // Close button anchored to the right edge of the header, vertically
    // centred. The hit-test area is a fixed square; the actual glyph is
    // drawn centred inside it during paint().
    const float close_x = bounds.x + bounds.w - kCloseSz - kPadX;
    const float close_y = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
    close_rect_ = {close_x, close_y, kCloseSz, kCloseSz};

    // One row per thread, stacked below the header. Rows clip at the
    // bottom of the panel — callers can scroll later (Task 8); for now
    // we just lay them out and let paint() handle the visible window via
    // the canvas clip.
    row_rects_.clear();
    row_rects_.reserve(threads_.size());
    float y = bounds.y + kHeaderH;
    for (std::size_t i = 0; i < threads_.size(); ++i)
    {
        row_rects_.push_back({bounds.x, y, bounds.w, kRowH});
        y += kRowH;
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void ThreadListView::paint(tk::PaintCtx& ctx)
{
    auto& cv         = ctx.canvas;
    const auto& pal  = ctx.theme.palette;

    // Panel background.
    cv.fill_rect(bounds_, pal.bg);

    // Header strip with title + close button.
    cv.fill_rect(header_rect_, pal.chrome_bg);

    // Bottom border separating header from rows.
    cv.fill_rect({header_rect_.x, header_rect_.bottom() - 1.0f,
                  header_rect_.w, 1.0f},
                 pal.separator);

    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, close_rect_.x - header_rect_.x - kPadX * 2.0f);
        auto title = ctx.factory.build_text("Threads", st);
        if (title)
        {
            const tk::Size sz = title->measure();
            const float ty = header_rect_.y + (kHeaderH - sz.h) * 0.5f;
            cv.draw_text(*title,
                         {header_rect_.x + kPadX, ty},
                         pal.text_primary);
        }
    }

    // Close-button background tint when pressed.
    if (press_close_)
    {
        cv.fill_rounded_rect(close_rect_, 4.0f, pal.subtle_pressed);
    }

    // Close glyph: a centered "×" (multiplication sign, U+00D7).
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

    // Rows.
    for (std::size_t i = 0; i < threads_.size(); ++i)
    {
        const auto& t   = threads_[i];
        const tk::Rect r = row_rects_[i];

        // Pressed-row highlight.
        if (static_cast<int>(i) == press_row_)
        {
            cv.fill_rect(r, pal.subtle_pressed);
        }

        // Per-row separator at the bottom.
        cv.fill_rect({r.x, r.bottom() - 1.0f, r.w, 1.0f}, pal.separator);

        // Right-side reply count, reserve its width so the previews don't
        // overlap it.
        char count_buf[48];
        std::snprintf(count_buf, sizeof(count_buf),
                      t.num_replies == 1 ? "%llu reply" : "%llu replies",
                      static_cast<unsigned long long>(t.num_replies));
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Small;
        cs.wrap = false;
        auto count_layout = ctx.factory.build_text(count_buf, cs);
        float count_w = 0.0f;
        if (count_layout)
        {
            const tk::Size sz = count_layout->measure();
            count_w = sz.w;
            const float cx = r.x + r.w - kPadX - sz.w;
            const float cy = r.y + (kRowH - sz.h) * 0.5f;
            cv.draw_text(*count_layout, {cx, cy}, pal.text_secondary);
        }

        const float text_left   = r.x + kPadX;
        const float text_right  = r.x + r.w - kPadX
                                  - count_w
                                  - (count_w > 0.0f ? 8.0f : 0.0f);
        const float text_max_w  = std::max(0.0f, text_right - text_left);

        // Top line: "<root_sender_name>: <root_body snippet>".
        {
            std::string preview;
            if (!t.root_sender_name.empty())
            {
                preview = t.root_sender_name + ": ";
            }
            preview += truncate_utf8(t.root_body, 80);

            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.wrap      = false;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            auto layout  = ctx.factory.build_text(preview, st);
            if (layout)
            {
                const tk::Size sz = layout->measure();
                cv.draw_text(*layout,
                             {text_left, r.y + kPadY},
                             pal.text_primary);
                (void)sz;
            }
        }

        // Bottom line: "↳ <latest_sender_name>: <latest_body snippet>"
        // — only when a reply exists.
        if (!t.latest_sender_name.empty())
        {
            std::string preview = "\xE2\x86\xB3 "; // U+21B3 ↳
            preview += t.latest_sender_name;
            preview += ": ";
            preview += truncate_utf8(t.latest_body, 80);

            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.wrap      = false;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            auto layout  = ctx.factory.build_text(preview, st);
            if (layout)
            {
                const tk::Size sz = layout->measure();
                const float ly = r.y + kRowH - kPadY - sz.h;
                cv.draw_text(*layout,
                             {text_left, ly},
                             pal.text_secondary);
            }
        }
    }

    // Near-bottom hook: fire once when the last row is fully laid out
    // inside the visible bounds. Cheap heuristic; Task 8 can replace
    // this with real scroll-aware bookkeeping if needed.
    if (!row_rects_.empty() && on_near_bottom)
    {
        const float last_bottom = row_rects_.back().bottom();
        if (last_bottom <= bounds_.bottom() + 1.0f)
        {
            // Don't actually fire from paint — paint should be idempotent.
            // The hook will be triggered by arrange() later. For now this
            // is a no-op: Task 8 wires real scrolling.
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ThreadListView::on_pointer_down(tk::Point local)
{
    // `local` is in widget-local coords (Point - bounds.origin). Convert
    // back to world space for our cached rects which are world-space.
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};

    if (rect_contains(close_rect_, w))
    {
        press_close_ = true;
        press_row_   = -1;
        return true;
    }

    for (std::size_t i = 0; i < row_rects_.size(); ++i)
    {
        if (rect_contains(row_rects_[i], w))
        {
            press_row_   = static_cast<int>(i);
            press_close_ = false;
            return true;
        }
    }
    return false;
}

void ThreadListView::on_pointer_up(tk::Point local, bool /*inside_self*/)
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
        return;
    }

    if (press_row_ >= 0)
    {
        const int idx = press_row_;
        press_row_    = -1;
        if (idx < static_cast<int>(row_rects_.size()) &&
            rect_contains(row_rects_[static_cast<std::size_t>(idx)], w) &&
            idx < static_cast<int>(threads_.size()))
        {
            if (on_thread_clicked)
            {
                on_thread_clicked(threads_[static_cast<std::size_t>(idx)]
                                      .root_event_id);
            }
        }
    }
}

} // namespace tesseract::views
