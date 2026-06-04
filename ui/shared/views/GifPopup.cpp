#include "views/GifPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

void GifPopup::set_results(std::vector<tesseract::GifResult> results)
{
    results_ = std::move(results);
    status_.clear();
    // Preselect the first result so Enter sends the top hit without arrowing.
    selected_index_ = results_.empty() ? -1 : 0;
    hovered_index_ = -1;
    pressed_index_ = -1;
    scroll_x_ = 0.0f;
    ensure_selected_visible_();
}

void GifPopup::set_status(std::string message)
{
    status_ = std::move(message);
    results_.clear();
    selected_index_ = -1;
    hovered_index_ = -1;
    pressed_index_ = -1;
    scroll_x_ = 0.0f;
}

void GifPopup::set_selected_index(int index)
{
    selected_index_ = index;
    ensure_selected_visible_();
}

bool GifPopup::move_selection(int delta)
{
    int n = visible_count();
    if (n <= 0)
    {
        return false;
    }
    int cur = selected_index_ < 0 ? 0 : selected_index_;
    selected_index_ = std::clamp(cur + delta, 0, n - 1);
    ensure_selected_visible_();
    return true;
}

float GifPopup::content_width_() const
{
    int n = visible_count();
    if (n <= 0)
    {
        return 0.0f;
    }
    return float(n) * kCellW + float(n - 1) * kGap;
}

void GifPopup::ensure_selected_visible_()
{
    int n = visible_count();
    if (n <= 0 || selected_index_ < 0)
    {
        scroll_x_ = 0.0f;
        return;
    }
    // Inner (scrollable) viewport width inside the side padding.
    const float inner_w = std::max(0.0f, bounds_.w - 2.0f * kPad);
    const float max_scroll = std::max(0.0f, content_width_() - inner_w);

    // Selected cell's left/right within the (unscrolled) content strip.
    const float left = float(selected_index_) * (kCellW + kGap);
    const float right = left + kCellW;
    if (left - scroll_x_ < 0.0f)
    {
        scroll_x_ = left; // selection ran off the left edge
    }
    else if (right - scroll_x_ > inner_w)
    {
        scroll_x_ = right - inner_w; // ran off the right edge
    }
    scroll_x_ = std::clamp(scroll_x_, 0.0f, max_scroll);
}

tk::Rect GifPopup::cell_rect(int i) const
{
    float x = bounds_.x + kPad + float(i) * (kCellW + kGap) - scroll_x_;
    float y = bounds_.y + kPad;
    return {x, y, kCellW, kCellH};
}

int GifPopup::cell_at(tk::Point local) const
{
    // Ignore hits outside the scrollable viewport so partially-scrolled cells
    // under the side padding don't register.
    if (local.x < bounds_.x + kPad || local.x > bounds_.x + bounds_.w - kPad)
    {
        return -1;
    }
    int n = visible_count();
    for (int i = 0; i < n; ++i)
    {
        tk::Rect r = cell_rect(i);
        if (local.x >= r.x && local.x < r.x + r.w && local.y >= r.y &&
            local.y < r.y + r.h)
        {
            return i;
        }
    }
    return -1;
}

tk::Size GifPopup::content_size(float max_width) const
{
    if (!results_.empty())
    {
        const float full = 2.0f * kPad + content_width_();
        const float w = max_width > 0.0f ? std::min(full, max_width) : full;
        const float h = 2.0f * kPad + kCellH + kAttribH;
        return {w, h};
    }
    if (!status_.empty())
    {
        // Heuristic width from the message length (no factory here to measure);
        // clamped to a sensible minimum and the host's available width.
        float w = 2.0f * kPad + 7.0f * float(status_.size());
        w = std::max(w, kStatusMinW);
        if (max_width > 0.0f)
        {
            w = std::min(w, max_width);
        }
        return {w, 2.0f * kPad + kStatusH};
    }
    return {0.0f, 0.0f};
}

tk::Size GifPopup::measure(tk::LayoutCtx&, tk::Size available)
{
    return content_size(available.w);
}

void GifPopup::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    ensure_selected_visible_();
}

void GifPopup::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    int n = visible_count();

    // Status-message mode: a single centered line, no cells / attribution.
    if (n <= 0)
    {
        if (status_.empty())
        {
            return;
        }
        ctx.canvas.fill_rect(bounds_, pal.bg);
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        if (auto tl = ctx.factory.build_text(status_, st))
        {
            tk::Size tsz = tl->measure();
            float tx = bounds_.x + (bounds_.w - tsz.w) * 0.5f;
            float ty = bounds_.y + (bounds_.h - tsz.h) * 0.5f;
            ctx.canvas.draw_text(*tl, {tx, ty}, pal.text_secondary);
        }
        ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f},
                             pal.separator);
        ctx.canvas.fill_rect(
            {bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
            pal.separator);
        ctx.canvas.fill_rect({bounds_.x, bounds_.y, 1.0f, bounds_.h},
                             pal.separator);
        ctx.canvas.fill_rect(
            {bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h},
            pal.separator);
        return;
    }

    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Clip the cell row to the scrollable viewport so overflowing / partially
    // scrolled cells don't paint over the side padding, border or attribution.
    ctx.canvas.push_clip_rect(
        {bounds_.x + kPad, bounds_.y + kPad,
         std::max(0.0f, bounds_.w - 2.0f * kPad), kCellH});
    for (int i = 0; i < n; ++i)
    {
        tk::Rect cell = cell_rect(i);
        const bool sel = (i == selected_index_);
        const bool hov = (i == hovered_index_);

        // Cell backing — selected/hovered tint shows through transparent PNGs
        // and fills the letterbox bars around landscape GIFs.
        ctx.canvas.fill_rect(cell, sel ? pal.sidebar_selected
                                       : hov ? pal.subtle_hover
                                             : pal.chrome_bg);

        const tk::Image* img =
            image_provider_ ? image_provider_(results_[std::size_t(i)].preview_url)
                            : nullptr;
        if (img)
        {
            float iw = static_cast<float>(img->width());
            float ih = static_cast<float>(img->height());
            if (iw > 0.0f && ih > 0.0f)
            {
                float sc = std::min(cell.w / iw, cell.h / ih);
                float dw = iw * sc, dh = ih * sc;
                ctx.canvas.draw_image(*img, {cell.x + (cell.w - dw) * 0.5f,
                                             cell.y + (cell.h - dh) * 0.5f, dw,
                                             dh});
            }
        }

        // Selection border.
        if (sel)
        {
            const float t = 2.0f;
            ctx.canvas.fill_rect({cell.x, cell.y, cell.w, t}, pal.accent);
            ctx.canvas.fill_rect({cell.x, cell.y + cell.h - t, cell.w, t},
                                 pal.accent);
            ctx.canvas.fill_rect({cell.x, cell.y, t, cell.h}, pal.accent);
            ctx.canvas.fill_rect({cell.x + cell.w - t, cell.y, t, cell.h},
                                 pal.accent);
        }
    }
    ctx.canvas.pop_clip();

    // Attribution — required by the provider's ToS. Bottom-right, secondary.
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        auto tl = ctx.factory.build_text("Powered by KLIPY", st);
        if (tl)
        {
            tk::Size tsz = tl->measure();
            float lx = bounds_.x + bounds_.w - kPad - tsz.w;
            float ly = bounds_.y + bounds_.h - kAttribH +
                       (kAttribH - tsz.h) * 0.5f;
            ctx.canvas.draw_text(*tl, {lx, ly}, pal.text_secondary);
        }
    }

    // 1px border around the whole strip.
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f}, pal.separator);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
                         pal.separator);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, 1.0f, bounds_.h}, pal.separator);
    ctx.canvas.fill_rect({bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h},
                         pal.separator);
}

bool GifPopup::on_pointer_down(tk::Point local)
{
    pressed_index_ = cell_at(local);
    return pressed_index_ >= 0;
}

void GifPopup::on_pointer_up(tk::Point local, bool /*inside_self*/)
{
    int up = cell_at(local);
    if (up >= 0 && up == pressed_index_ && on_accepted)
    {
        on_accepted(results_[std::size_t(up)]);
    }
    pressed_index_ = -1;
}

bool GifPopup::on_pointer_move(tk::Point local)
{
    int h = cell_at(local);
    if (h != hovered_index_)
    {
        hovered_index_ = h;
        return true;
    }
    return false;
}

void GifPopup::on_pointer_leave()
{
    hovered_index_ = -1;
}

} // namespace tesseract::views
