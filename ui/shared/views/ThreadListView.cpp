#include "ThreadListView.h"
#include "media_utils.h"

#include "tk/theme.h"

#include <algorithm>
#include <cstdio>
#include <memory>

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

ThreadListView::ThreadListView()
{
    // Added as a child so dispatch_pointer_down walks it before our own
    // on_pointer_down sees the row hit-test, and paint() renders it on top.
    auto close = std::make_unique<tk::Button>(
        "\xC3\x97", // U+00D7 ×
        std::function<void()>{}, tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] {
        if (on_close) on_close();
    });
}

void ThreadListView::set_threads(std::vector<tesseract::ThreadInfo> threads)
{
    threads_ = std::move(threads);
    // Drop any in-flight press: the row indices it refers to may now
    // point at a different thread (or nothing).
    press_row_ = -1;
    // Rebuild row_rects_ immediately so paint() can safely index it even
    // if arrange() hasn't run yet for this new thread count. arrange()
    // will correct positions whenever the bounds change.
    row_rects_.clear();
    row_rects_.reserve(threads_.size());
    float y = bounds_.y + kHeaderH;
    for (std::size_t i = 0; i < threads_.size(); ++i)
    {
        row_rects_.push_back({bounds_.x, y, bounds_.w, kRowH});
        y += kRowH;
    }
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

    if (close_btn_)
    {
        const float cx = bounds.x + bounds.w - kCloseSz - kCloseInset;
        const float cy = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
        close_btn_->arrange(lc, {cx, cy, kCloseSz, kCloseSz});
    }

    // Rows sit below the empty header strip and clip at the bottom of
    // the panel — callers can scroll later; for now we just lay them
    // out and let paint() handle the visible window via the canvas clip.
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

        const float text_left  = r.x + kPadX;
        const float text_right = r.x + r.w - kPadX
                                 - count_w
                                 - (count_w > 0.0f ? 8.0f : 0.0f);
        const float text_max_w = std::max(0.0f, text_right - text_left);

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
                cv.draw_text(*layout,
                             {text_left, r.y + kPadY},
                             pal.text_primary);
            }
        }

        // Bottom line: "↳ <latest_sender_name>: <latest_body snippet>" —
        // only when a reply exists.
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

    // Children (the floating close button) paint last so they sit above
    // the rows.
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
            cv.draw_text(*glyph, {gx, gy}, pal.text_secondary);
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ThreadListView::on_pointer_down(tk::Point local)
{
    // `dispatch_pointer_down` already walked into the close-button child;
    // if we reach this method the click landed somewhere else, so it can
    // only be a row hit. `local` is in widget-local coords — convert back
    // to world space for our cached rects which are world-space.
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};

    for (std::size_t i = 0; i < row_rects_.size(); ++i)
    {
        if (rect_contains(row_rects_[i], w))
        {
            press_row_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

void ThreadListView::on_pointer_up(tk::Point local, bool /*inside_self*/)
{
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};

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
