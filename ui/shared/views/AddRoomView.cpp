#include "AddRoomView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kARHeaderPadX = 16.0f;
constexpr float kARTabH = 32.0f;
constexpr float kARCardRadius = 10.0f;

bool ar_hit(const tk::Rect& r, tk::Point p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

} // namespace

AddRoomView::AddRoomView()
{
    tk::Widget::set_visible(false);

    auto jr = tk::create_widget<JoinRoomView>(this);
    jr->set_title_visible(false);
    jr->set_visible(false);
    jr->on_cancel = [this] { close(); };
    join_view_ = add_child(std::move(jr));

    auto cr = tk::create_widget<CreateRoomView>(this);
    cr->set_title_visible(false);
    cr->set_visible(false);
    cr->on_cancel = [this] { close(); };
    create_view_ = add_child(std::move(cr));

    auto tv = tk::create_widget<tk::TabView>(this);
    tv->set_items({tk::tr("Join"), tk::tr("Create")});
    tv->on_selected = [this](int idx)
    {
        set_active_tab(idx == 0 ? Tab::Join : Tab::Create);
    };
    tab_view_ = add_child(std::move(tv));
}

void AddRoomView::open(Tab initial_tab)
{
    is_open_ = true;
    active_tab_ = initial_tab;
    press_outside_ = false;
    set_visible(true);
    if (tab_view_) tab_view_->set_selected_index(active_tab_ == Tab::Join ? 0 : 1);

    if (active_tab_ == Tab::Join)
    {
        if (create_view_) create_view_->set_visible(false);
        if (join_view_) join_view_->open();
    }
    else
    {
        if (join_view_) join_view_->set_visible(false);
        if (create_view_)
        {
            create_view_->reset();
            create_view_->set_visible(true);
        }
    }
    pending_focus_ = true;
    if (host()) host()->request_relayout();
}

void AddRoomView::open_join_with_prefill(const std::string& prefill)
{
    is_open_ = true;
    active_tab_ = Tab::Join;
    press_outside_ = false;
    set_visible(true);
    if (tab_view_) tab_view_->set_selected_index(0);

    if (create_view_) create_view_->set_visible(false);
    if (join_view_) join_view_->open(prefill);

    pending_focus_ = true;
    if (host()) host()->request_relayout();
}

void AddRoomView::close()
{
    if (!is_open_)
        return;
    is_open_ = false;
    pending_focus_ = false;
    set_visible(false);
    press_outside_ = false;
    if (join_view_) join_view_->close();
    if (create_view_) create_view_->set_visible(false);
    if (on_close) on_close();
}

void AddRoomView::set_active_tab(Tab t)
{
    if (!is_open_ || t == active_tab_)
        return;
    active_tab_ = t;
    // Sync unconditionally here rather than only at the tab_view_->on_selected
    // call site, so active_tab_ and tab_view_'s own selection stay in
    // lockstep regardless of who called set_active_tab() — a no-op re-sync
    // (set_selected_index() no-ops when already equal) when this originated
    // from the tab view's own click/keypress, but load-bearing for any other
    // caller (e.g. a future keyboard shortcut) that bypasses tab_view_ entirely.
    if (tab_view_) tab_view_->set_selected_index(t == Tab::Join ? 0 : 1);

    if (t == Tab::Join)
    {
        // set_visible(false) directly, not close() — switching tabs must
        // not fire on_close and tear down the whole dialog.
        if (create_view_) create_view_->set_visible(false);
        if (join_view_) join_view_->open();
    }
    else
    {
        if (join_view_) join_view_->set_visible(false);
        if (create_view_)
        {
            create_view_->reset();
            create_view_->set_visible(true);
        }
    }
    pending_focus_ = true;
    if (host()) host()->request_relayout();
}

void AddRoomView::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v)
    {
        if (join_view_) join_view_->set_visible(false);
        if (create_view_) create_view_->set_visible(false);
    }
}

tk::Size AddRoomView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void AddRoomView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const float cw = kCardW;
    const float ch = kCardH;
    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    const float cy = bounds.y + (bounds.h - ch) * 0.5f;
    card_rect_ = {cx, cy, cw, ch};

    const tk::Rect header_bounds{cx + kARHeaderPadX, cy + (kHeaderH - kARTabH) * 0.5f,
                                 cw - kARHeaderPadX * 2.0f, kARTabH};
    if (tab_view_) tab_view_->arrange(ctx, header_bounds);

    const tk::Rect content_bounds{cx, cy + kHeaderH, cw, ch - kHeaderH};
    if (active_tab_ == Tab::Join)
    {
        if (join_view_) join_view_->arrange(ctx, content_bounds);
    }
    else
    {
        if (create_view_) create_view_->arrange(ctx, content_bounds);
    }
}

void AddRoomView::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;

    // See JoinRoomView::pending_focus_'s doc comment: open()/set_active_tab()
    // defer this because the active child's native field overlay isn't
    // positioned until arrange() has run.
    if (pending_focus_)
    {
        pending_focus_ = false;
        if (active_tab_ == Tab::Join && join_view_)
            join_view_->focus_alias_field();
        else if (create_view_)
            create_view_->focus_name_field();
    }

    const auto& pal = ctx.theme.palette;

    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));
    ctx.canvas.fill_rounded_rect(card_rect_, kARCardRadius, pal.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, kARCardRadius, pal.popup_border, 1.0f);

    ctx.canvas.fill_rect(
        {card_rect_.x, card_rect_.y + kHeaderH - 1.0f, card_rect_.w, 1.0f},
        pal.separator);

    if (tab_view_) tab_view_->paint(ctx);

    if (active_tab_ == Tab::Join && join_view_)
        join_view_->paint(ctx);
    else if (create_view_)
        create_view_->paint(ctx);
}

bool AddRoomView::on_pointer_down(tk::Point local)
{
    if (!is_open_)
        return false;

    // Reached only when no child (tab_view_/join_view_/create_view_)
    // claimed the click — dispatch_pointer_down already tried them all,
    // topmost-first. So this is either card padding/chrome or the backdrop.
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    press_outside_ = !ar_hit(card_rect_, world);
    return true; // always consume — modal backdrop
}

void AddRoomView::on_pointer_up(tk::Point /*local*/, bool inside_self)
{
    if (!is_open_)
        return;

    if (press_outside_ && inside_self)
    {
        press_outside_ = false;
        close();
        return;
    }
    press_outside_ = false;
}

bool AddRoomView::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/, bool /*is_touchpad*/)
{
    // dispatch_wheel already tried any scrollable child (neither JoinRoomView
    // nor CreateRoomView have one today); modal overlay — eat the event so
    // it doesn't fall through to the room list scrolling behind the dialog.
    return is_open_;
}

} // namespace tesseract::views
