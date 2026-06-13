#include "RoomSearchBar.h"

#include "icons.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

constexpr float kPadX   = 12.0f; // outer horizontal margin
constexpr float kBtnSize = 28.0f; // icon button size
constexpr float kBtnGap  =  4.0f; // gap between buttons
constexpr float kFieldH  = 28.0f; // native text field height within strip
constexpr float kIconPx  = 16.0f; // glyph render size

} // namespace

RoomSearchBar::RoomSearchBar()
{
    // Count label — leftmost, right-aligned text showing "3 of 12" etc.
    auto lbl = std::make_unique<tk::Label>("", tk::FontRole::Small);
    lbl->set_halign(tk::TextHAlign::Leading);
    lbl->set_trim(tk::TextTrim::Ellipsis);
    lbl->set_visible(false);
    count_label_ = add_child(std::move(lbl));

    // UP button — navigate to older match (delta = -1).
    auto up = std::make_unique<tk::Button>("", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    up->set_on_click([this] { if (on_navigate) on_navigate(-1); });
    up->set_visible(false);
    up_btn_ = add_child(std::move(up));

    // DOWN button — navigate to newer match (delta = +1).
    auto dn = std::make_unique<tk::Button>("", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    dn->set_on_click([this] { if (on_navigate) on_navigate(+1); });
    dn->set_visible(false);
    down_btn_ = add_child(std::move(dn));

    // Paginate checkbox.
    auto cb = std::make_unique<tk::CheckButton>("Paginate", false);
    cb->set_font_role(tk::FontRole::Small);
    cb->on_change = [this](bool v) { if (on_paginate_toggled) on_paginate_toggled(v); };
    cb->set_visible(false);
    paginate_cb_ = add_child(std::move(cb));

    // Close button.
    auto cl = std::make_unique<tk::Button>("", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    cl->set_on_click([this] { if (on_close) on_close(); });
    cl->set_visible(false);
    close_btn_ = add_child(std::move(cl));
}

void RoomSearchBar::open()
{
    is_open_ = true;
    query_.clear();
    count_text_ = "Type to search";

    if (count_label_) { count_label_->set_text(count_text_); count_label_->set_visible(true); }
    if (up_btn_)      up_btn_->set_visible(true);
    if (down_btn_)    down_btn_->set_visible(true);
    if (paginate_cb_) paginate_cb_->set_visible(true);
    if (close_btn_)   close_btn_->set_visible(true);
    // paginate_cb_ checked state is intentionally preserved across re-opens.
}

void RoomSearchBar::close()
{
    is_open_ = false;
    field_rect_        = {};
    count_label_max_w_ = 0.0f;

    if (count_label_) count_label_->set_visible(false);
    if (up_btn_)      up_btn_->set_visible(false);
    if (down_btn_)    down_btn_->set_visible(false);
    if (paginate_cb_) paginate_cb_->set_visible(false);
    if (close_btn_)   close_btn_->set_visible(false);
}

void RoomSearchBar::set_query(const std::string& q)
{
    if (q == query_)
        return;
    query_ = q;
    if (on_query_changed)
        on_query_changed(query_);
}

void RoomSearchBar::set_match_status(int current, int total, bool searching,
                                     bool at_start)
{
    if (searching)
    {
        count_text_ = "Searching…";
    }
    else if (at_start && total == 0)
    {
        count_text_ = "Start of conversation";
    }
    else if (total == 0)
    {
        count_text_ = "No matches";
    }
    else
    {
        count_text_ = std::to_string(current) + " of " + std::to_string(total);
    }

    if (count_label_)
        count_label_->set_text(count_text_);
}

bool RoomSearchBar::paginate_enabled() const
{
    return paginate_cb_ && paginate_cb_->checked();
}

tk::Size RoomSearchBar::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kStripH};
}

void RoomSearchBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    if (!is_open_)
    {
        field_rect_ = {};
        // Arrange children at zero rects so hit-testing returns nothing.
        if (count_label_) count_label_->arrange(ctx, {});
        if (up_btn_)      up_btn_->arrange(ctx, {});
        if (down_btn_)    down_btn_->arrange(ctx, {});
        if (paginate_cb_) paginate_cb_->arrange(ctx, {});
        if (close_btn_)   close_btn_->arrange(ctx, {});
        return;
    }

    const float mid_y = bounds.y + (kStripH - kBtnSize) * 0.5f;

    // Layout right-to-left:
    // [field] [kCountW label] [kBtnGap] [up] [kBtnGap] [down] [kBtnGap] [paginate_cb] [kBtnGap] [close]

    // Close button: far right
    const tk::Rect close_r{bounds.x + bounds.w - kPadX - kBtnSize,
                           mid_y, kBtnSize, kBtnSize};
    if (close_btn_)
        close_btn_->arrange(ctx, close_r);

    // Paginate checkbox: measure its natural width then place left of close.
    float paginate_w = 90.0f; // fallback
    if (paginate_cb_)
    {
        // Width=0 → CheckButton returns natural (box + gap + label) width.
        // ceil + 4px buffer prevents sub-pixel rounding from triggering the
        // ellipsis when arrange re-constrains to exactly that width.
        const tk::Size m = paginate_cb_->measure(ctx, {0.0f, kStripH});
        paginate_w = std::ceil(m.w) + 4.0f;
    }
    const float paginate_right = close_r.x - kBtnGap;
    const tk::Rect paginate_r{paginate_right - paginate_w,
                              bounds.y + (kStripH - kBtnSize) * 0.5f,
                              paginate_w, kBtnSize};
    if (paginate_cb_)
        paginate_cb_->arrange(ctx, paginate_r);

    // DOWN button: left of paginate
    const tk::Rect down_r{paginate_r.x - kBtnGap - kBtnSize,
                          mid_y, kBtnSize, kBtnSize};
    if (down_btn_)
        down_btn_->arrange(ctx, down_r);

    // UP button: left of down
    const tk::Rect up_r{down_r.x - kBtnGap - kBtnSize,
                        mid_y, kBtnSize, kBtnSize};
    if (up_btn_)
        up_btn_->arrange(ctx, up_r);

    // Count label: measure natural width but only ever grow the reserved slot
    // so the text field doesn't jitter as the match count changes during pagination.
    const float count_right = up_r.x - kBtnGap;
    float count_w = 0.0f;
    if (count_label_)
    {
        const tk::Size lsz = count_label_->measure(ctx, {0.0f, kStripH});
        count_label_max_w_ = std::max(count_label_max_w_, std::ceil(lsz.w) + 4.0f);
        count_w = count_label_max_w_;
        const tk::Rect count_r{count_right - count_w,
                               bounds.y + (kStripH - lsz.h) * 0.5f,
                               count_w, lsz.h};
        count_label_->arrange(ctx, count_r);
    }

    // Native text field rect: from left margin to just left of count label.
    // field_w must account for bounds.x so the field does not overlap the
    // right-hand controls when the room panel has a non-zero x offset.
    const float field_left = bounds.x + kPadX;
    const float field_w    = std::max(0.0f, count_right - count_w - field_left - kBtnGap);
    field_rect_ = {field_left,
                   bounds.y + (kStripH - kFieldH) * 0.5f,
                   field_w, kFieldH};
}

void RoomSearchBar::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;

    const auto& pal = ctx.theme.palette;
    const tk::Rect b = bounds();

    // Strip background.
    ctx.canvas.fill_rect(b, pal.chrome_bg);

    // Bottom hairline separator.
    ctx.canvas.fill_rect({b.x, b.bottom() - 1.0f, b.w, 1.0f}, pal.separator);

    // Decorative border around the native text field area.
    if (field_rect_.w > 0.0f && field_rect_.h > 0.0f)
        ctx.canvas.stroke_rect(field_rect_, pal.border, 1.0f);

    // Child widgets paint their own backgrounds/text.
    if (count_label_ && count_label_->visible()) count_label_->paint(ctx);
    if (up_btn_ && up_btn_->visible())           up_btn_->paint(ctx);
    if (down_btn_ && down_btn_->visible())       down_btn_->paint(ctx);
    if (paginate_cb_ && paginate_cb_->visible()) paginate_cb_->paint(ctx);
    if (close_btn_ && close_btn_->visible())     close_btn_->paint(ctx);

    // Icon glyphs drawn on top of buttons' hover/press backgrounds.
    if (up_btn_ && up_btn_->visible())
        up_icon_.draw(ctx.canvas, ctx.factory, kChevronUpSvg,
                      up_btn_->bounds(), kIconPx, pal.text_primary);
    if (down_btn_ && down_btn_->visible())
        down_icon_.draw(ctx.canvas, ctx.factory, kChevronDownSvg,
                        down_btn_->bounds(), kIconPx, pal.text_primary);
    if (close_btn_ && close_btn_->visible())
        close_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg,
                         close_btn_->bounds(), kIconPx, pal.text_muted);
}

} // namespace tesseract::views
