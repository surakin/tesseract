#include "text_field.h"

#include <algorithm>

namespace tk
{

TextField::TextField(Host& host, float min_height)
    : Label("", FontRole::Body), host_(&host), min_height_(min_height)
{
    set_halign(TextHAlign::Leading);
    set_min_size({0.0f, min_height_});

    field_ = host_->make_text_field();
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
                host_->request_focus(this);
            else if (host_->focused_widget() == this)
                host_->clear_focus();
            syncing_from_native_ = false;
        });

    // Forward Tab/Shift-Tab into canvas traversal. Reuses the existing
    // popup-nav mechanism rather than a new callback type — safe to install
    // permanently since nothing else drives popup-nav on a plain field.
    // Everything else (Up/Down/Escape/Left/Right) falls through to
    // whatever's been layered on via push_popup_nav(), most-recent first.
    field_->set_on_popup_nav(
        [this](NavKey nk) -> bool
        {
            if (nk == NavKey::Tab)      return host_->advance_focus(true);
            if (nk == NavKey::ShiftTab) return host_->advance_focus(false);
            for (auto it = nav_handlers_.rbegin(); it != nav_handlers_.rend(); ++it)
                if ((*it)(nk)) return true;
            return false;
        });
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
    Widget::set_visible(v);
    if (field_) field_->set_visible(v);
}

void TextField::set_focused(bool focused)
{
    if (focused)
        host_->request_focus(this);
    else if (host_->focused_widget() == this)
        host_->clear_focus();
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

} // namespace tk
