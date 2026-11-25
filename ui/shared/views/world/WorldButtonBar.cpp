#include "views/world/WorldButtonBar.h"

#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/shortcut_registry.h"

#include <algorithm>

namespace tesseract::views::world
{

namespace
{
constexpr float kBtnW = 112.0f;
constexpr float kBtnH = 30.0f;
constexpr float kGap = 6.0f;
constexpr float kPad = 6.0f;
constexpr float kHandleW = 40.0f;
constexpr float kHandleH = 24.0f;
} // namespace

WorldButtonBar::WorldButtonBar(std::vector<Entry> entries)
{
    // A small tab at the edge; its text is the same open or closed.
    handle_ = add_child(tk::create_widget<tk::Button>(this, tk::tr("…"), std::function<void()>{},
                                                      tk::Button::Variant::Subtle));
    handle_->set_accessible_name(tk::tr("Show world actions"));
    handle_->set_on_click(
        [this]
        {
            toggle();
        });
    for (Entry& e : entries)
    {
        auto* b = add_child(tk::create_widget<tk::Button>(this, e.label, std::function<void()>{},
                                                          tk::Button::Variant::Subtle));
        b->set_accessible_name(e.label);
        b->set_enabled(e.enabled);
        b->set_visible(false);
        b->set_on_click(
            [this, id = e.id]
            {
                if (on_activate)
                {
                    on_activate(id);
                }
            });
        items_.push_back({std::move(e.id), b});
    }
}

std::string WorldButtonBar::access_name() const
{
    return tk::tr("World actions");
}

void WorldButtonBar::set_expanded(bool on, bool focus_first)
{
    if (on == expanded_)
    {
        if (on && focus_first)
        {
            // Already open: just bring the keyboard in.
            for (const Item& it : items_)
            {
                if (it.button->enabled())
                {
                    if (host())
                    {
                        host()->request_focus(it.button);
                    }
                    break;
                }
            }
        }
        return;
    }
    const bool had_focus_inside = !on && focus_is_inside_();
    expanded_ = on;
    handle_->set_accessible_name(on ? tk::tr("Hide world actions") : tk::tr("Show world actions"));
    for (const Item& it : items_)
    {
        it.button->set_visible(on);
    }
    if (on && focus_first && host())
    {
        for (const Item& it : items_)
        {
            if (it.button->enabled())
            {
                host()->request_focus(it.button);
                break;
            }
        }
    }
    else if (had_focus_inside && host())
    {
        host()->request_focus(handle_); // the focused entry just vanished
    }
    if (on_layout_changed)
    {
        on_layout_changed();
    }
    else if (host())
    {
        host()->request_relayout();
    }
}

bool WorldButtonBar::focus_is_inside_() const
{
    if (!host())
    {
        return false;
    }
    for (const tk::Widget* w = host()->focused_widget(); w; w = w->parent())
    {
        if (w == this)
        {
            return true;
        }
    }
    return false;
}

void WorldButtonBar::set_entry_enabled(const std::string& id, bool enabled)
{
    if (tk::Button* b = entry_button(id))
    {
        b->set_enabled(enabled);
        if (!enabled && host() && host()->focused_widget() == b)
        {
            host()->request_focus(handle_);
        }
    }
}

bool WorldButtonBar::entry_enabled(const std::string& id) const
{
    const tk::Button* b = entry_button(id);
    return b && b->enabled();
}

tk::Button* WorldButtonBar::entry_button(const std::string& id) const
{
    for (const Item& it : items_)
    {
        if (it.id == id)
        {
            return it.button;
        }
    }
    return nullptr;
}

tk::Size WorldButtonBar::measure(tk::LayoutCtx&, tk::Size)
{
    if (!expanded_ || items_.empty())
    {
        return {kHandleW, kHandleH};
    }
    const auto n = static_cast<float>(items_.size());
    return {kBtnW + 2.0f * kPad, kHandleH + kPad + n * kBtnH + (n - 1.0f) * kGap + kPad};
}

void WorldButtonBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    handle_->arrange(ctx, {bounds.x, bounds.y, kHandleW, kHandleH});
    float y = bounds.y + kHandleH + kPad;
    for (const Item& it : items_)
    {
        if (expanded_)
        {
            it.button->arrange(ctx, {bounds.x + kPad, y, kBtnW, kBtnH});
            y += kBtnH + kGap;
        }
        else
        {
            it.button->arrange(ctx, {});
        }
    }
}

void WorldButtonBar::paint_before_children(tk::PaintCtx& ctx)
{
    // The handle sits over a 3D picture of any colour: a backdrop of the chrome colour keeps it
    // (and its theme-coloured text) readable, closed or open.
    ctx.canvas.fill_rounded_rect({bounds_.x, bounds_.y, kHandleW, kHandleH}, 6.0f,
                                 ctx.theme.palette.chrome_bg);
    if (expanded_ && bounds_.h > kHandleH)
    {
        ctx.canvas.fill_rect({bounds_.x, bounds_.y + kHandleH, bounds_.w, bounds_.h - kHandleH},
                             ctx.theme.palette.chrome_bg);
    }
}

bool WorldButtonBar::on_key_down(const tk::KeyEvent& e)
{
    // Only while the keyboard is on the bar: a "B" typed anywhere else in the window is not ours
    // (the world view that carries the bar handles the key for itself).
    if (!focus_is_inside_())
    {
        return false;
    }
    if (matches(ShortcutId::WorldBar, e))
    {
        const bool was_open = expanded_;
        toggle(/*focus_first=*/true);
        if (was_open && on_closed_by_key)
        {
            on_closed_by_key();
        }
        return true;
    }
    if (expanded_ && e.key == tk::Key::Escape && !e.ctrl && !e.alt && !e.meta)
    {
        set_expanded(false);
        if (on_closed_by_key)
        {
            on_closed_by_key();
        }
        return true;
    }
    return false;
}

} // namespace tesseract::views::world
