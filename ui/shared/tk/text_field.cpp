#include "text_field.h"

#include <algorithm>

namespace tk
{

TextField::TextField(float min_height)
    : Label("", FontRole::Body), min_height_(min_height)
{
    set_halign(TextHAlign::Leading);
    set_min_size({0.0f, min_height_});

    field_ = host()->make_text_field();
    if (!field_)
        return; // e.g. a test Host with no native backend — stay a plain spacer

    // Sync canvas focus when the user clicks directly into the native
    // field — that click bypasses canvas hit-testing entirely (the native
    // overlay eats it at the OS level), so nothing else would notice.
    // syncing_from_native_ tells on_focus_lost() not to push set_focused()
    // back down to a field that just told us it changed on its own — see
    // the comment on on_focus_lost() in the header.
    field_->set_on_focus_changed(
        [this](bool now_focused)
        {
            syncing_from_native_ = true;
            if (now_focused)
                host()->request_focus(this);
            else if (host()->focused_widget() == this)
                host()->clear_focus();
            syncing_from_native_ = false;
        });

    // Re-assert canvas focus on every native click, even one that doesn't
    // change OS focus (the field was already focused) — that's the only
    // case set_on_focus_changed above can't catch, and it's exactly the
    // click that needs to dismiss an open register_popup()'d popup (see
    // Host::request_focus()'s doc comment and
    // NativeTextField::set_on_pointer_down's). request_focus() is a no-op
    // when `this` is already the focused widget, so this is safe to fire on
    // every click.
    field_->set_on_pointer_down([this] { host()->request_focus(this); });

    // Forward Tab/Shift-Tab into canvas traversal. Reuses the existing
    // popup-nav mechanism rather than a new callback type — safe to install
    // permanently since nothing else drives popup-nav on a plain field.
    // Everything else (Up/Down/Escape/Left/Right) falls through to
    // whatever's been layered on via push_popup_nav(), most-recent first.
    field_->set_on_popup_nav(
        [this](NavKey nk) -> bool
        {
            if (nk == NavKey::Tab)      return host()->advance_focus(true);
            if (nk == NavKey::ShiftTab) return host()->advance_focus(false);
            for (auto it = nav_handlers_.rbegin(); it != nav_handlers_.rend(); ++it)
                if ((*it)(nk)) return true;
            return false;
        });

    // Canvas-drawn-text backends (see NativeTextField::rendered_image())
    // report here, with the rect that changed, whenever their offscreen
    // buffer changes so the surface repaints and picks up the new pixels.
    // request_repaint_rect() falls back to a full request_repaint() on
    // backends that don't support a scoped native repaint (see its doc
    // comment in host.h). No-op on backends that still composite on screen
    // directly.
    field_->set_on_repaint_needed([this](Rect r) { host()->request_repaint_rect(r); });
}

void TextField::set_text(std::string text)
{
    if (field_) field_->set_text(std::move(text));
}

std::string TextField::text() const
{
    return field_ ? field_->text() : std::string{};
}

void TextField::set_placeholder(std::string text)
{
    if (field_) field_->set_placeholder(std::move(text));
}

void TextField::set_password(bool password)
{
    if (field_) field_->set_password(password);
}

void TextField::set_compact(bool compact)
{
    if (field_) field_->set_compact(compact);
}

void TextField::set_text_color(Color c)
{
    if (field_) field_->set_text_color(c);
}

void TextField::set_on_changed(std::function<void(const std::string&)> cb)
{
    if (field_) field_->set_on_changed(std::move(cb));
}

void TextField::set_on_submit(std::function<void()> cb)
{
    if (field_) field_->set_on_submit(std::move(cb));
}

void TextField::set_on_focus_changed(std::function<void(bool)> cb)
{
    on_focus_changed_cb_ = std::move(cb);
}

void TextField::push_popup_nav(std::function<bool(NavKey)> cb)
{
    nav_handlers_.push_back(std::move(cb));
}

void TextField::pop_popup_nav()
{
    if (!nav_handlers_.empty())
        nav_handlers_.pop_back();
}

void TextField::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (field_) field_->set_enabled(enabled);
}

void TextField::set_visible(bool v)
{
    if (v == Widget::visible()) return; // no-op — see header comment
    Widget::set_visible(v);
    if (field_) field_->set_visible(v);
}

void TextField::set_focused(bool focused)
{
    if (focused)
        host()->request_focus(this);
    else if (host()->focused_widget() == this)
        host()->clear_focus();
}

void TextField::arrange(LayoutCtx& ctx, Rect bounds)
{
    Label::arrange(ctx, bounds);
    if (!field_)
        return;
    float h = std::max(bounds_.h, min_height_);
    Rect r{bounds_.x + overlay_inset_,
           bounds_.y - (h - bounds_.h) * 0.5f + overlay_inset_,
           bounds_.w - overlay_inset_ * 2.0f,
           h - overlay_inset_ * 2.0f};
    field_->set_rect(r);
}

void TextField::paint(PaintCtx& ctx)
{
    // text_ is always "" here (the real text lives in the native field) —
    // Label::paint() is a no-op draw in that case, called anyway to keep
    // parity with a plain Label for the no-native-backend test-Host case.
    Label::paint(ctx);
    if (!field_)
        return;
    // Forward the inherited background (see Widget::background_color()) to
    // the native backend so an offscreen capture (rendered_image() below)
    // can pre-fill an opaque buffer instead of a transparent one — see
    // NativeTextField::set_background_color()'s doc comment in host.h.
    // Only fires on an actual change so it's cheap to check every paint().
    if (auto bg = background_color(); bg && bg != last_bg_pushed_)
    {
        field_->set_background_color(*bg);
        last_bg_pushed_ = bg;
    }
    if (const tk::Image* img = field_->rendered_image())
    {
        // See NativeTextField::rendered_image_rect()'s doc comment — several
        // backends narrow the native control to its own natural height and
        // centre it within bounds_, so rendered_image() may not cover the
        // full widget rect. A zero-size rect means the backend doesn't do
        // that narrowing; bounds_ is already correct in that case.
        Rect draw_rect = field_->rendered_image_rect();
        if (draw_rect.w <= 0.0f || draw_rect.h <= 0.0f)
            draw_rect = bounds_;
        ctx.canvas.draw_image(*img, draw_rect);
    }
    // Canvas-owned caret (see NativeTextField::caret_owned_by_canvas()'s doc
    // comment in host.h) — drawn separately from rendered_image() above
    // since a decoupled backend's captured image never contains it, by
    // design (that's what lets the blink timer skip recapturing the whole
    // control just to toggle this). A minimum 1-DIP width guards against a
    // backend reporting a technically-zero-width caret rect.
    if (field_->caret_owned_by_canvas() && field_->caret_blink_visible())
    {
        Rect cr = field_->caret_rect();
        if (cr.h > 0.0f)
        {
            cr.w = std::max(cr.w, 1.0f);
            ctx.canvas.fill_rect(cr, ctx.theme.palette.text_primary);
        }
    }
}

bool TextField::on_pointer_down(Point local)
{
    if (!field_)
        return false;
    field_->forward_pointer_down({bounds_.x + local.x, bounds_.y + local.y});
    return true;
}

void TextField::on_pointer_drag(Point local)
{
    if (field_)
        field_->forward_pointer_drag({bounds_.x + local.x, bounds_.y + local.y});
}

void TextField::on_pointer_up(Point local, bool)
{
    if (field_)
        field_->forward_pointer_up({bounds_.x + local.x, bounds_.y + local.y});
}

bool TextField::on_pointer_move(Point)
{
    if (field_)
        field_->set_hovering(true);
    return false;
}

void TextField::on_pointer_leave()
{
    if (field_)
        field_->set_hovering(false);
}

} // namespace tk
