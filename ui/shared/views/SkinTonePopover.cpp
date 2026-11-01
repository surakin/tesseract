#include "SkinTonePopover.h"

#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

using tesseract::emoji::SkinTone;

// kSkinTones order.
constexpr const char* kToneNames[] = {
    tk::N_("Default skin tone"), tk::N_("Light skin tone"),       tk::N_("Medium-light skin tone"),
    tk::N_("Medium skin tone"),  tk::N_("Medium-dark skin tone"), tk::N_("Dark skin tone"),
};

} // namespace

// A real tk::Button (hover/press/focus/click/accessibility) whose content is
// the emoji glyph at the picker's cell size, plus a ring on the current tone.
class SkinTonePopover::ToneButton : public tk::Button
{
protected:
    ToneButton() : tk::Button({}, {}, tk::Button::Variant::Subtle)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(ToneButton)

public:
    void set_glyph(std::string glyph)
    {
        glyph_ = std::move(glyph);
        layout_.reset();
    }
    const std::string& glyph() const
    {
        return glyph_;
    }
    void set_current(bool current)
    {
        current_ = current;
    }
    // Position without a LayoutCtx (open() runs from input handlers, not a
    // layout pass); a Button has no children, so this is all arrange() does.
    void place(tk::Rect r)
    {
        bounds_ = r;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        tk::Button::paint(ctx);
        if (!layout_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::EmojiPickerCell;
            layout_ = ctx.factory.build_glyph(glyph_, st);
        }
        if (layout_)
        {
            // Centered by hand, as in EmojiPicker::paint_cell.
            tk::Size sz = layout_->measure();
            ctx.canvas.draw_text(
                *layout_,
                {bounds_.x + (bounds_.w - sz.w) * 0.5f, bounds_.y + (bounds_.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }
        if (current_)
        {
            ctx.canvas.stroke_rounded_rect(bounds_, 4.0f, ctx.theme.palette.accent, 1.5f);
        }
    }

protected:
    bool paints_content() const override
    {
        return false;
    }

private:
    std::string glyph_;
    tk::CachedTextLayout layout_;
    bool current_ = false;
};

SkinTonePopover::SkinTonePopover()
{
    for (std::size_t i = 0; i < buttons_.size(); ++i)
    {
        auto b = tk::create_widget<ToneButton>(this);
        b->set_min_size({kButtonSize, kButtonSize});
        b->set_accessible_name(tk::tr(kToneNames[i]));
        b->set_on_click(
            [this, i]
            {
                pick_(static_cast<int>(i));
            });
        buttons_[i] = add_child(std::move(b));
    }
    set_visible(false);
}

void SkinTonePopover::open(std::string_view glyph, SkinTone current, tk::Rect anchor,
                           tk::Rect limits)
{
    for (std::size_t i = 0; i < buttons_.size(); ++i)
    {
        const SkinTone tone = tesseract::emoji::kSkinTones[i];
        buttons_[i]->set_glyph(std::string(tesseract::emoji::with_skin_tone(glyph, tone)));
        buttons_[i]->set_current(tone == current);
    }

    // Centered over the cell, above it unless that leaves the picker.
    float x = anchor.x + (anchor.w - kWidth) * 0.5f;
    float y = anchor.y - kHeight - 4.0f;
    if (y < limits.y)
    {
        y = anchor.y + anchor.h + 4.0f;
    }
    x = std::clamp(x, limits.x, std::max(limits.x, limits.x + limits.w - kWidth));
    y = std::clamp(y, limits.y, std::max(limits.y, limits.y + limits.h - kHeight));
    bounds_ = {x, y, kWidth, kHeight};
    // Placed now, not at the next paint, so hit-testing is right immediately.
    for (std::size_t i = 0; i < buttons_.size(); ++i)
    {
        buttons_[i]->place({bounds_.x + kPadding + static_cast<float>(i) * (kButtonSize + kGap),
                            bounds_.y + kPadding, kButtonSize, kButtonSize});
    }

    open_ = true;
    set_visible(true);
    focused_ = static_cast<int>(current);
    took_focus_ = false;
    prev_focus_.reset();
    if (auto* h = host())
    {
        tk::Widget* prev = h->focused_widget();
        if (prev && inside_owner_(prev))
        {
            prev_focus_ = tk::track(prev);
            took_focus_ = true;
            focus_(focused_);
        }
        h->request_repaint();
    }
}

bool SkinTonePopover::inside_owner_(const tk::Widget* w) const
{
    const tk::Widget* owner = parent();
    for (const tk::Widget* a = w; a; a = a->parent())
    {
        if (a == owner)
        {
            return true;
        }
    }
    return false;
}

void SkinTonePopover::sync_focused_from_host_()
{
    auto* h = host();
    if (!h)
    {
        return;
    }
    const auto it = std::find(buttons_.begin(), buttons_.end(), h->focused_widget());
    if (it != buttons_.end())
    {
        focused_ = static_cast<int>(it - buttons_.begin());
    }
}

void SkinTonePopover::close()
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    set_visible(false);
    if (auto* h = host())
    {
        const bool focus_in_menu =
            std::find(buttons_.begin(), buttons_.end(), h->focused_widget()) != buttons_.end();
        if (took_focus_ && focus_in_menu)
        {
            // Back to where the keyboard user was (the grid or the search
            // field), so typing / arrows keep working in the open picker.
            auto prev = prev_focus_.lock();
            if (prev && prev->visible_in_tree())
            {
                h->request_focus(prev.get());
            }
            else
            {
                h->clear_focus();
            }
        }
        h->request_repaint();
    }
    took_focus_ = false;
    prev_focus_.reset();
}

bool SkinTonePopover::handle_key(const tk::KeyEvent& e)
{
    if (!open_)
    {
        return false;
    }
    sync_focused_from_host_();
    switch (e.key)
    {
    case tk::Key::Escape:
        close();
        return true;
    case tk::Key::Left:
        focus_(std::max(0, focused_ - 1));
        return true;
    case tk::Key::Right:
        focus_(std::min(static_cast<int>(buttons_.size()) - 1, focused_ + 1));
        return true;
    case tk::Key::Enter:
    case tk::Key::Space:
        pick_(focused_);
        return true;
    default:
        return false;
    }
}

std::string SkinTonePopover::glyph_for(SkinTone tone) const
{
    return buttons_[static_cast<std::size_t>(tone)]->glyph();
}

std::string SkinTonePopover::access_name() const
{
    return tk::tr("Skin tone");
}

void SkinTonePopover::paint(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }
    ctx.canvas.fill_rounded_rect(bounds_, 8.0f, ctx.theme.palette.bg);
    ctx.canvas.stroke_rounded_rect(bounds_, 8.0f, ctx.theme.palette.popup_border, 1.0f);
    paint_children(ctx);
}

void SkinTonePopover::pick_(int index)
{
    if (index < 0 || index >= static_cast<int>(buttons_.size()))
    {
        return;
    }
    close();
    if (on_picked)
    {
        on_picked(tesseract::emoji::kSkinTones[static_cast<std::size_t>(index)]);
    }
}

void SkinTonePopover::focus_(int index)
{
    focused_ = index;
    if (!took_focus_)
    {
        return; // mouse-opened: leave focus where it is (see took_focus_)
    }
    if (auto* h = host())
    {
        h->request_focus(buttons_[static_cast<std::size_t>(index)]);
    }
}

} // namespace tesseract::views
