#include "StatusEditor.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <span>
#include <utility>

namespace tesseract::views
{

StatusEditor::StatusEditor()
{
    if (!host())
        return;

    // label_ "\xC3\x97" (×) is internal test scaffolding only — see
    // PronounsEditor's remove button; the visible glyph is the emoji itself
    // (Subtle variant) or a Lucide icon (Icon variant), swapped in
    // refresh_emoji_button_().
    auto emoji = tk::create_widget<tk::Button>(
        this, std::string{}, std::function<void()>{}, tk::Button::Variant::Icon);
    emoji->set_min_size({kEmojiW, kRowH});
    emoji->set_accessible_name(tk::tr("Choose status emoji"));
    emoji_btn_ = add_child(std::move(emoji));
    // Icon vs. glyph is set by refresh_emoji_button_() below / on every change.
    emoji_btn_->set_on_click(
        [this]
        {
            if (on_emoji_button_clicked)
                on_emoji_button_clicked(emoji_btn_->bounds());
        });

    auto clear = tk::create_widget<tk::Button>(
        this, "\xC3\x97", std::function<void()>{}, tk::Button::Variant::Icon);
    clear->set_min_size({kClearW, kRowH});
    clear->set_icon(kCloseSvg, 12.0f);
    clear->set_accessible_name(tk::tr("Clear status emoji"));
    clear_btn_ = add_child(std::move(clear));
    clear_btn_->set_on_click([this] { set_emoji(std::string{}); });

    auto text = tk::create_widget<tk::TextField>(this, kRowH);
    text->set_placeholder(tk::tr("What's your status?"));
    text_field_ = add_child(std::move(text));
    text_field_->set_on_submit([this] { notify_changed_(); });
    text_field_->set_on_focus_changed([this](bool focused)
                                      { if (!focused) flush(); });

    refresh_emoji_button_();
}

// ---- content --------------------------------------------------------------

void StatusEditor::set_status(std::string emoji, std::string text)
{
    emoji_ = std::move(emoji);
    refresh_emoji_button_();
    if (text_field_) text_field_->set_text(text);
    last_emoji_ = emoji_;
    last_text_  = std::move(text);
    have_last_  = true;
}

void StatusEditor::set_emoji(std::string glyph)
{
    if (glyph == emoji_)
        return;
    emoji_ = std::move(glyph);
    refresh_emoji_button_();
    notify_changed_();
}

std::string StatusEditor::status_text() const
{
    return text_field_ ? text_field_->text() : std::string();
}

tk::Widget* StatusEditor::emoji_button() const
{
    return emoji_btn_;
}

void StatusEditor::refresh_emoji_button_()
{
    if (!emoji_btn_)
        return;
    emoji_glyph_layout_.reset();
    if (emoji_.empty())
    {
        // No emoji → the button draws its own Lucide face icon.
        emoji_btn_->set_variant(tk::Button::Variant::Icon);
        emoji_btn_->set_icon(kEmojiSvg, 18.0f);
    }
    else
    {
        // Emoji set → a bare Subtle button (keeps the hover/press fill); the
        // glyph itself is hand-drawn in paint_after_children() at emoji-cell
        // size, since Button only renders its label in tiny body font.
        emoji_btn_->set_variant(tk::Button::Variant::Subtle);
        emoji_btn_->set_icon(std::span<const std::uint8_t>{});
    }
    if (clear_btn_)
        clear_btn_->set_visible(editable_ && !busy_ && !emoji_.empty()
                                && visible_in_tree());
}

// ---- state ---------------------------------------------------------------

void StatusEditor::set_editable(bool editable)
{
    editable_ = editable;
    const bool on = editable && !busy_;
    if (emoji_btn_) emoji_btn_->set_enabled(on);
    if (clear_btn_) clear_btn_->set_enabled(on);
    if (text_field_) text_field_->set_enabled(on);
    refresh_emoji_button_();
}

void StatusEditor::set_busy(bool busy)
{
    busy_ = busy;
    if (busy)
        error_.clear();
    error_layout_.reset();
    set_editable(editable_);
}

void StatusEditor::set_error(std::string error)
{
    error_ = std::move(error);
    error_layout_.reset();
}

void StatusEditor::set_visible(bool v)
{
    Widget::set_visible(v);
    if (emoji_btn_) emoji_btn_->set_visible(v);
    if (text_field_) text_field_->set_visible(v);
    refresh_emoji_button_(); // clear button also depends on emoji_ state
}

void StatusEditor::on_theme_changed(const tk::Theme& t)
{
    if (text_field_) text_field_->set_text_color(t.palette.text_primary);
    emoji_glyph_layout_.reset();
}

// ---- emit --------------------------------------------------------------

void StatusEditor::notify_changed_()
{
    const std::string emoji = emoji_;
    const std::string text  = status_text();
    if (have_last_ && emoji == last_emoji_ && text == last_text_)
        return;
    last_emoji_ = emoji;
    last_text_  = text;
    have_last_  = true;
    if (on_changed)
        on_changed(emoji, text);
}

void StatusEditor::flush()
{
    notify_changed_();
}

// ---- layout ------------------------------------------------------------

tk::Size StatusEditor::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float error_h = error_.empty() ? 0.0f : kErrorGap + kErrorH;
    return {constraints.w > 0 ? constraints.w : 0, kRowH + error_h};
}

void StatusEditor::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    error_layout_.reset();

    const float emoji_x = bounds.x;
    const float clear_x = emoji_x + kEmojiW + kFieldGap;
    const float text_x  = clear_x + kClearW + kFieldGap;
    const float max_text_w = std::max(0.0f, bounds.x + bounds.w - text_x);
    const float text_w = std::min(kTextW, max_text_w);

    const bool show = visible_in_tree();

    if (emoji_btn_)
    {
        emoji_btn_->set_visible(show);
        if (show)
            emoji_btn_->arrange(ctx, {emoji_x, bounds.y, kEmojiW, kRowH});
    }
    if (clear_btn_)
    {
        const bool cshow = show && editable_ && !busy_ && !emoji_.empty();
        clear_btn_->set_visible(cshow);
        if (cshow)
            clear_btn_->arrange(ctx, {clear_x, bounds.y, kClearW, kRowH});
    }
    if (text_field_)
    {
        text_field_->set_visible(show);
        if (show)
            text_field_->arrange(ctx, {text_x, bounds.y, text_w, kRowH});
    }
}

void StatusEditor::paint_after_children(tk::PaintCtx& ctx)
{
    // The chosen emoji, drawn over the (bare) button at emoji-cell size.
    if (emoji_btn_ && emoji_btn_->visible() && !emoji_.empty())
    {
        if (!emoji_glyph_layout_)
        {
            tk::TextStyle st;
            st.role = tk::FontRole::EmojiPickerCell;
            // build_glyph, not build_text — see EmojiPicker::paint_cell's
            // comment: this is a single whole-cell glyph, not text with
            // incidental emoji, so it should render (and measure, for the
            // hand-centering below) at EmojiPickerCell's own size.
            emoji_glyph_layout_ = ctx.factory.build_glyph(emoji_, st);
        }
        if (emoji_glyph_layout_)
        {
            const tk::Rect b  = emoji_btn_->bounds();
            const tk::Size sz = emoji_glyph_layout_->measure();
            const tk::Color c = (editable_ && !busy_)
                                    ? ctx.theme.palette.text_primary
                                    : ctx.theme.palette.text_muted;
            ctx.canvas.draw_text(*emoji_glyph_layout_,
                                 {b.x + (b.w - sz.w) * 0.5f,
                                  b.y + (b.h - sz.h) * 0.5f},
                                 c);
        }
    }

    if (error_.empty())
        return;

    if (!error_layout_)
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.max_width = bounds_.w;
        error_layout_ = ctx.factory.build_text(error_, st);
    }
    if (error_layout_)
    {
        ctx.canvas.draw_text(*error_layout_,
                             {bounds_.x, bounds_.y + kRowH + kErrorGap},
                             tk::Color::rgb(0xcc3333));
    }
}

} // namespace tesseract::views
