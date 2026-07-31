#include "RoomSearchBar.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

constexpr float kRoomSearchBarPadX   = 12.0f; // outer horizontal margin
constexpr float kBtnSize = 28.0f; // icon button size
constexpr float kRoomSearchBarBtnGap  =  4.0f; // gap between buttons
constexpr float kRoomSearchBarFieldH  = 28.0f; // native text field height within strip
constexpr float kIconPx  = 16.0f; // glyph render size

} // namespace

RoomSearchBar::RoomSearchBar()
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kRoomSearchBarFieldH);
        field->set_placeholder(tk::tr("Find in conversation\xe2\x80\xa6"));
        field->set_on_changed([this](const std::string& q) { set_query(q); });
        field->set_visible(false);
        search_field_ = add_child(std::move(field));
    }

    // Count label — leftmost, right-aligned text showing "3 of 12" etc.
    auto lbl = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    lbl->set_halign(tk::TextHAlign::Leading);
    lbl->set_trim(tk::TextTrim::Ellipsis);
    lbl->set_visible(false);
    count_label_ = add_child(std::move(lbl));

    // UP button — navigate to older match (delta = -1).
    auto up = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    up->set_on_click([this] { if (on_navigate) on_navigate(-1); });
    up->set_accessible_name(tk::tr("Previous match"));
    up->set_visible(false);
    up_btn_ = add_child(std::move(up));
    up_btn_->set_icon(kChevronUpSvg, kIconPx);

    // DOWN button — navigate to newer match (delta = +1).
    auto dn = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    dn->set_on_click([this] { if (on_navigate) on_navigate(+1); });
    dn->set_accessible_name(tk::tr("Next match"));
    dn->set_visible(false);
    down_btn_ = add_child(std::move(dn));
    down_btn_->set_icon(kChevronDownSvg, kIconPx);

    // Paginate checkbox.
    auto cb = tk::create_widget<tk::CheckButton>(this, tk::tr("Paginate"), false);
    cb->set_font_role(tk::FontRole::Small);
    cb->on_change = [this](bool v) { if (on_paginate_toggled) on_paginate_toggled(v); };
    cb->set_visible(false);
    paginate_cb_ = add_child(std::move(cb));

    // Close button.
    auto cl = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                           tk::Button::Variant::Icon);
    cl->set_on_click([this] { if (on_close) on_close(); });
    cl->set_accessible_name(tk::tr("Close"));
    cl->set_visible(false);
    close_btn_ = add_child(std::move(cl));
    close_btn_->set_icon(kCloseSvg, kIconPx);
}

void RoomSearchBar::open()
{
    is_open_ = true;
    query_.clear();
    count_text_ = tk::tr("Type to search");

    if (count_label_) { count_label_->set_text(count_text_); count_label_->set_visible(true); }
    if (up_btn_)      up_btn_->set_visible(true);
    if (down_btn_)    down_btn_->set_visible(true);
    if (paginate_cb_) paginate_cb_->set_visible(show_paginate_);
    if (close_btn_)   close_btn_->set_visible(show_close_button_);
    if (search_field_)
    {
        search_field_->set_visible(true);
        search_field_->set_text("");
        search_field_->set_focused(true);
    }
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
    if (search_field_) search_field_->set_visible(false);
}

void RoomSearchBar::set_query(const std::string& q)
{
    if (q == query_)
        return;
    query_ = q;
    if (on_query_changed)
        on_query_changed(query_);
}

void RoomSearchBar::clear_query()
{
    if (search_field_)
        search_field_->set_text("");
    set_query("");
}

void RoomSearchBar::set_show_close_button(bool show)
{
    show_close_button_ = show;
    if (close_btn_)
        close_btn_->set_visible(show && is_open_);
}

void RoomSearchBar::set_show_paginate(bool show)
{
    show_paginate_ = show;
    if (paginate_cb_)
        paginate_cb_->set_visible(show && is_open_);
}

void RoomSearchBar::on_theme_changed(const tk::Theme& t)
{
    // search_field_ sits directly on the strip fill (paint() only strokes
    // an outline around field_rect_, no separate inset fill) — see
    // Widget::background_color()'s doc comment.
    set_background_color(t.palette.chrome_bg);
    if (search_field_)
        search_field_->set_text_color(t.palette.text_primary);
    if (close_btn_)
        close_btn_->set_icon_color_override(t.palette.text_muted);
}

void RoomSearchBar::set_match_status(int current, int total, bool searching,
                                     bool at_start)
{
    if (searching)
    {
        count_text_ = tk::tr("Searching…");
    }
    else if (at_start && total == 0)
    {
        count_text_ = tk::tr("Start of conversation");
    }
    else if (total == 0)
    {
        count_text_ = tk::tr("No matches");
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
        if (search_field_) search_field_->set_visible(false);
        return;
    }

    const float mid_y = bounds.y + (kStripH - kBtnSize) * 0.5f;

    // Layout right-to-left:
    // [field] [kCountW label] [kRoomSearchBarBtnGap] [up] [kRoomSearchBarBtnGap] [down] [kRoomSearchBarBtnGap] [paginate_cb] [kRoomSearchBarBtnGap] [close]
    //
    // right_edge tracks where the next element's right side should land.
    // close/paginate only advance it past their width+gap when shown, so a
    // hidden one's reserved space is genuinely reclaimed by whatever's next
    // in the chain, rather than left as an empty gap.
    float right_edge = bounds.x + bounds.w - kRoomSearchBarPadX;

    // Close button: far right, when shown.
    if (show_close_button_)
    {
        const tk::Rect close_r{right_edge - kBtnSize, mid_y, kBtnSize, kBtnSize};
        if (close_btn_)
            close_btn_->arrange(ctx, close_r);
        right_edge = close_r.x - kRoomSearchBarBtnGap;
    }
    else if (close_btn_)
    {
        close_btn_->arrange(ctx, {});
    }

    // Paginate checkbox: measure its natural width then place left of
    // whatever's to its right, when shown.
    if (show_paginate_)
    {
        float paginate_w = 90.0f; // fallback
        if (paginate_cb_)
        {
            // Width=0 → CheckButton returns natural (box + gap + label) width.
            // ceil + 4px buffer prevents sub-pixel rounding from triggering the
            // ellipsis when arrange re-constrains to exactly that width.
            const tk::Size m = paginate_cb_->measure(ctx, {0.0f, kStripH});
            paginate_w = std::ceil(m.w) + 4.0f;
        }
        const tk::Rect paginate_r{right_edge - paginate_w,
                                  bounds.y + (kStripH - kBtnSize) * 0.5f,
                                  paginate_w, kBtnSize};
        if (paginate_cb_)
            paginate_cb_->arrange(ctx, paginate_r);
        right_edge = paginate_r.x - kRoomSearchBarBtnGap;
    }
    else if (paginate_cb_)
    {
        paginate_cb_->arrange(ctx, {});
    }

    // DOWN button: left of whatever's to its right.
    const tk::Rect down_r{right_edge - kBtnSize, mid_y, kBtnSize, kBtnSize};
    if (down_btn_)
        down_btn_->arrange(ctx, down_r);
    right_edge = down_r.x - kRoomSearchBarBtnGap;

    // UP button: left of down.
    const tk::Rect up_r{right_edge - kBtnSize, mid_y, kBtnSize, kBtnSize};
    if (up_btn_)
        up_btn_->arrange(ctx, up_r);
    right_edge = up_r.x - kRoomSearchBarBtnGap;

    // Count label: measure natural width but only ever grow the reserved slot
    // so the text field doesn't jitter as the match count changes during pagination.
    float count_w = 0.0f;
    if (count_label_)
    {
        const tk::Size lsz = count_label_->measure(ctx, {0.0f, kStripH});
        count_label_max_w_ = std::max(count_label_max_w_, std::ceil(lsz.w) + 4.0f);
        count_w = count_label_max_w_;
        const tk::Rect count_r{right_edge - count_w,
                               bounds.y + (kStripH - lsz.h) * 0.5f,
                               count_w, lsz.h};
        count_label_->arrange(ctx, count_r);
    }
    right_edge -= count_w;

    // Native text field rect: from left margin to just left of count label.
    // field_w must account for bounds.x so the field does not overlap the
    // right-hand controls when the room panel has a non-zero x offset.
    const float field_left = bounds.x + kRoomSearchBarPadX;
    const float field_w    = std::max(0.0f, right_edge - field_left - kRoomSearchBarBtnGap);
    field_rect_ = {field_left,
                   bounds.y + (kStripH - kRoomSearchBarFieldH) * 0.5f,
                   field_w, kRoomSearchBarFieldH};

    if (search_field_)
    {
        search_field_->set_visible(true);
        search_field_->arrange(ctx, field_rect_);
    }
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

    // Child widgets paint their own backgrounds/text/icons.
    if (search_field_ && search_field_->visible()) search_field_->paint(ctx);
    if (count_label_ && count_label_->visible()) count_label_->paint(ctx);
    if (up_btn_ && up_btn_->visible())           up_btn_->paint(ctx);
    if (down_btn_ && down_btn_->visible())       down_btn_->paint(ctx);
    if (paginate_cb_ && paginate_cb_->visible()) paginate_cb_->paint(ctx);
    if (close_btn_ && close_btn_->visible())     close_btn_->paint(ctx);
}

} // namespace tesseract::views
