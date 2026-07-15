#include "text_area.h"

#include <algorithm>

namespace tk
{

TextArea::TextArea(Host& host, float min_height)
    : Label("", FontRole::Body), host_(&host), min_height_(min_height)
{
    set_halign(TextHAlign::Leading);
    set_min_size({0.0f, min_height_});

    area_ = host_->make_text_area();
    if (!area_)
        return; // e.g. a test Host with no native backend — stay a plain spacer

    // Sync canvas focus when the user clicks directly into the native
    // area — that click bypasses canvas hit-testing entirely (the native
    // overlay eats it at the OS level), so nothing else would notice. See
    // tk::TextField's constructor for the syncing_from_native_ rationale.
    area_->set_on_focus_changed(
        [this](bool now_focused)
        {
            syncing_from_native_ = true;
            if (now_focused)
                host_->request_focus(this);
            else if (host_->focused_widget() == this)
                host_->clear_focus();
            syncing_from_native_ = false;
        });

    // Unlike tk::TextField (which always claims Tab/ShiftTab for canvas
    // traversal before consulting its handler stack), TextArea gives the
    // pushed-handler stack first refusal on every key, including
    // Tab/ShiftTab — the compose bar's popup controllers (mention/slash/
    // shortcode/gif) claim Tab/ShiftTab themselves for suggestion-cycling.
    // Falls back to canvas traversal only once nothing in the stack
    // consumes the key.
    area_->set_on_popup_nav(
        [this](NavKey nk) -> bool
        {
            for (auto it = nav_handlers_.rbegin(); it != nav_handlers_.rend(); ++it)
                if ((*it)(nk)) return true;
            if (nk == NavKey::Tab)      return host_->advance_focus(true);
            if (nk == NavKey::ShiftTab) return host_->advance_focus(false);
            return false;
        });
}

void TextArea::set_text(std::string text)
{
    if (area_) area_->set_text(std::move(text));
}

std::string TextArea::text() const
{
    return area_ ? area_->text() : std::string{};
}

void TextArea::set_placeholder(std::string text)
{
    if (area_) area_->set_placeholder(std::move(text));
}

void TextArea::set_text_color(Color c)
{
    if (area_) area_->set_text_color(c);
}

void TextArea::set_font_role(FontRole role)
{
    if (area_) area_->set_font_role(role);
}

bool TextArea::visible() const
{
    return area_ ? area_->visible() : false;
}

float TextArea::natural_height() const
{
    return area_ ? area_->natural_height() : min_height_;
}

void TextArea::set_on_height_changed(std::function<void(float)> cb)
{
    if (area_) area_->set_on_height_changed(std::move(cb));
}

void TextArea::set_on_changed(std::function<void(const std::string&)> cb)
{
    if (area_) area_->set_on_changed(std::move(cb));
}

void TextArea::set_on_submit(std::function<void()> cb)
{
    if (area_) area_->set_on_submit(std::move(cb));
}

void TextArea::set_on_focus_changed(std::function<void(bool)> cb)
{
    on_focus_changed_cb_ = std::move(cb);
}

void TextArea::insert_at_cursor(std::string text)
{
    if (area_) area_->insert_at_cursor(std::move(text));
}

Rect TextArea::cursor_rect() const
{
    return area_ ? area_->cursor_rect() : Rect{};
}

void TextArea::replace_range(int start, int end, std::string text)
{
    if (area_) area_->replace_range(start, end, std::move(text));
}

int TextArea::cursor_byte_pos() const
{
    return area_ ? area_->cursor_byte_pos() : 0;
}

void TextArea::insert_mention(int start, int end, const std::string& user_id,
                              const std::string& display_name, bool is_room)
{
    if (area_) area_->insert_mention(start, end, user_id, display_name, is_room);
}

void TextArea::insert_emoticon(int start, int end, const std::string& shortcode,
                               const std::string& mxc_url, const Image* image)
{
    if (area_) area_->insert_emoticon(start, end, shortcode, mxc_url, image);
}

std::vector<tesseract::MentionSeg> TextArea::composer_draft() const
{
    return area_ ? area_->composer_draft() : std::vector<tesseract::MentionSeg>{};
}

void TextArea::set_mention_colors(Color bg, Color fg)
{
    if (area_) area_->set_mention_colors(bg, fg);
}

void TextArea::set_on_edit_last(std::function<bool()> cb)
{
    if (area_) area_->set_on_edit_last(std::move(cb));
}

void TextArea::set_on_image_paste(NativeTextArea::ImagePasteHandler cb)
{
    if (area_) area_->set_on_image_paste(std::move(cb));
}

void TextArea::set_image_resolver(std::function<const Image*(const std::string& uri)> cb)
{
    if (area_) area_->set_image_resolver(std::move(cb));
}

void TextArea::push_popup_nav(std::function<bool(NavKey)> cb)
{
    nav_handlers_.push_back(std::move(cb));
}

void TextArea::pop_popup_nav()
{
    if (!nav_handlers_.empty())
        nav_handlers_.pop_back();
}

void TextArea::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (area_) area_->set_enabled(enabled);
}

void TextArea::set_visible(bool v)
{
    Widget::set_visible(v);
    if (area_) area_->set_visible(v);
}

void TextArea::set_focused(bool focused)
{
    if (focused)
        host_->request_focus(this);
    else if (host_->focused_widget() == this)
        host_->clear_focus();
}

void TextArea::arrange(LayoutCtx& ctx, Rect bounds)
{
    Label::arrange(ctx, bounds);
    if (!area_)
        return;
    float h = std::max(bounds_.h, min_height_);
    Rect r{bounds_.x, bounds_.y - (h - bounds_.h) * 0.5f, bounds_.w, h};
    area_->set_rect(r);
}

} // namespace tk
