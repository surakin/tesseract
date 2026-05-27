#include "PinnedBanner.h"

#include "media_utils.h"  // rect_contains
#include "tk/theme.h"

namespace tesseract::views
{

namespace
{

// UTF-8-safe truncation (mirrors ThreadListView::truncate_utf8): clip to
// `max_bytes` bytes, then back off until we're past the next UTF-8 start
// byte so we never split a code-point. Folds newlines to single spaces
// because previews are single-line.
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

PinnedBanner::PinnedBanner() = default;

void PinnedBanner::set_pins(std::vector<tesseract::PinnedEvent> pins)
{
    pins_ = std::move(pins);
    if (pins_.empty())
    {
        current_index_ = 0;
    }
    else if (current_index_ >= pins_.size())
    {
        current_index_ = pins_.size() - 1;
    }
    // Drop any in-flight press: the rects it referred to are stale.
    press_body_ = press_up_ = press_down_ = false;
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size PinnedBanner::measure(tk::LayoutCtx& /*ctx*/, tk::Size c)
{
    // Zero-height when empty so RoomView's arrange skips us cleanly.
    return tk::Size{c.w, pins_.empty() ? 0.0f : kBannerH};
}

void PinnedBanner::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);
    if (pins_.empty())
    {
        body_rect_ = up_rect_ = down_rect_ = {};
        return;
    }
    // Chevrons stacked vertically on the right; body fills the rest.
    const float right = bounds.x + bounds.w;
    up_rect_   = {right - kChevronPad - kChevronSz,
                  bounds.y + 2.0f,
                  kChevronSz, kChevronSz};
    down_rect_ = {right - kChevronPad - kChevronSz,
                  bounds.y + 2.0f + kChevronSz,
                  kChevronSz, kChevronSz};
    // Leave room for the chevron column + counter strip on the right.
    const float reserved = kChevronSz + 2.0f * kChevronPad + kCounterW;
    body_rect_ = {bounds.x, bounds.y,
                  bounds.w - reserved, bounds.h};
}

// ── paint ─────────────────────────────────────────────────────────────────

void PinnedBanner::paint(tk::PaintCtx& ctx)
{
    if (pins_.empty()) return;

    auto&       cv  = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Banner background uses chrome_bg so it visually reads as a strip of
    // chrome rather than chat content. A 1px separator at the bottom keeps
    // it visually distinct from the message list below.
    cv.fill_rect(bounds_, pal.chrome_bg);
    cv.fill_rect({bounds_.x, bounds_.bottom() - 1.0f, bounds_.w, 1.0f},
                 pal.separator);

    // Press-state highlight on the body — gives click-feedback before the
    // jump fires on pointer_up.
    if (press_body_)
    {
        cv.fill_rect(body_rect_, pal.subtle_pressed);
    }

    const auto& p = pins_[current_index_];

    // Compose "<sender>: <body>" preview, then truncate UTF-8-safely.
    std::string preview = p.sender_name;
    if (!p.body_preview.empty())
    {
        if (!preview.empty()) preview += ": ";
        preview += p.body_preview;
    }
    preview = truncate_utf8(std::move(preview), 80);

    tk::TextStyle body_style{};
    body_style.role = tk::FontRole::Body;
    body_style.wrap = false;
    body_style.trim = tk::TextTrim::Ellipsis;
    body_style.max_width = body_rect_.w - 2.0f * kPadX;
    auto body_layout = ctx.factory.build_text(preview, body_style);
    if (body_layout)
    {
        const tk::Size sz = body_layout->measure();
        const float    ty = body_rect_.y + (body_rect_.h - sz.h) * 0.5f;
        cv.draw_text(*body_layout, {body_rect_.x + kPadX, ty},
                     pal.text_primary);
    }

    tk::TextStyle small_style{};
    small_style.role = tk::FontRole::Small;
    small_style.wrap = false;

    // Counter strip ("i/n") + chevrons only when more than one pin exists.
    if (pins_.size() > 1)
    {
        const std::string counter = std::to_string(current_index_ + 1) + "/" +
                                    std::to_string(pins_.size());
        auto counter_layout = ctx.factory.build_text(counter, small_style);
        if (counter_layout)
        {
            const tk::Size sz = counter_layout->measure();
            const float    cx = body_rect_.x + body_rect_.w +
                                (kCounterW - sz.w) * 0.5f;
            const float    cy = bounds_.y + (bounds_.h - sz.h) * 0.5f;
            cv.draw_text(*counter_layout, {cx, cy}, pal.text_secondary);
        }

        // Pressed-chevron highlight rectangles.
        if (press_up_)   cv.fill_rect(up_rect_,   pal.subtle_pressed);
        if (press_down_) cv.fill_rect(down_rect_, pal.subtle_pressed);

        // Simple triangle glyphs — matches other glyph-based icons used in
        // the toolkit; can be swapped for hand-drawn vectors later.
        auto up_layout = ctx.factory.build_text("\xE2\x96\xB2", small_style); // ▲
        if (up_layout)
        {
            const tk::Size sz = up_layout->measure();
            cv.draw_text(*up_layout,
                         {up_rect_.x + (up_rect_.w - sz.w) * 0.5f,
                          up_rect_.y + (up_rect_.h - sz.h) * 0.5f},
                         pal.text_secondary);
        }
        auto dn_layout = ctx.factory.build_text("\xE2\x96\xBC", small_style); // ▼
        if (dn_layout)
        {
            const tk::Size sz = dn_layout->measure();
            cv.draw_text(*dn_layout,
                         {down_rect_.x + (down_rect_.w - sz.w) * 0.5f,
                          down_rect_.y + (down_rect_.h - sz.h) * 0.5f},
                         pal.text_secondary);
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool PinnedBanner::on_pointer_down(tk::Point local)
{
    if (pins_.empty()) return false;
    // `local` is widget-local; our cached rects are world-space.
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    if (pins_.size() > 1 && rect_contains(up_rect_, world))
    {
        press_up_ = true;
        return true;
    }
    if (pins_.size() > 1 && rect_contains(down_rect_, world))
    {
        press_down_ = true;
        return true;
    }
    if (rect_contains(body_rect_, world))
    {
        press_body_ = true;
        return true;
    }
    return false;
}

void PinnedBanner::on_pointer_up(tk::Point local, bool /*inside_self*/)
{
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    if (press_up_)
    {
        press_up_ = false;
        if (rect_contains(up_rect_, world) && current_index_ > 0)
        {
            --current_index_;
        }
        return;
    }
    if (press_down_)
    {
        press_down_ = false;
        if (rect_contains(down_rect_, world) &&
            current_index_ + 1 < pins_.size())
        {
            ++current_index_;
        }
        return;
    }
    if (press_body_)
    {
        press_body_ = false;
        if (rect_contains(body_rect_, world) &&
            current_index_ < pins_.size())
        {
            if (on_jump_to)
            {
                on_jump_to(pins_[current_index_].event_id);
            }
        }
        return;
    }
}

} // namespace tesseract::views
