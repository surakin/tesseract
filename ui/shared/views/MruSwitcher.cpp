#include "MruSwitcher.h"

#include "room_chip_strip.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kCardMargin = 40.0f;
} // namespace

MruSwitcher::MruSwitcher()
{
    tk::Widget::set_visible(false);
}

void MruSwitcher::set_recent_provider(RoomsProvider p)
{
    recent_provider_ = std::move(p);
}

void MruSwitcher::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void MruSwitcher::begin_cycle()
{
    rooms_ = recent_provider_ ? recent_provider_()
                              : std::vector<tesseract::RoomInfo>{};
    if (rooms_.size() < 2)
    {
        // Nothing to switch to — matches Alt-Tab with no other window.
        is_open_ = false;
        set_visible(false);
        return;
    }

    selected_ = 1;
    pressed_chip_ = -1;
    press_outside_ = false;
    is_open_ = true;
    set_visible(true);
    if (host())
    {
        host()->request_relayout();
    }
}

void MruSwitcher::advance(int delta)
{
    if (!is_open_ || rooms_.empty())
    {
        return;
    }
    const int n = static_cast<int>(rooms_.size());
    selected_ = ((selected_ + delta) % n + n) % n;
    if (host())
    {
        host()->request_repaint();
    }
}

void MruSwitcher::commit()
{
    if (!is_open_)
    {
        return;
    }
    if (selected_ >= 0 && static_cast<std::size_t>(selected_) < rooms_.size())
    {
        const std::string room_id = rooms_[static_cast<std::size_t>(selected_)].id;
        close_();
        if (on_room_selected)
        {
            on_room_selected(room_id);
        }
        return;
    }
    close_();
}

void MruSwitcher::cancel()
{
    if (!is_open_)
    {
        return;
    }
    close_();
}

void MruSwitcher::close_()
{
    is_open_ = false;
    selected_ = -1;
    rooms_.clear();
    pressed_chip_ = -1;
    press_outside_ = false;
    set_visible(false);
}

// ── Layout + paint ────────────────────────────────────────────────────────

tk::Size MruSwitcher::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void MruSwitcher::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;

    // Size the card to fit every snapshotted room at once — unlike
    // QuickSwitcher's search results, the strip never scrolls, so a fixed
    // width would silently truncate the tail of a longer MRU list (see
    // paint_room_chips()'s max_fit clamp). kCardW is the ceiling (comfortably
    // fits recent_room_ids_'s cap of 8); the window-bounds clamp still
    // applies below it for a narrow window.
    constexpr float kChipPadX = 12.0f;
    const float n = static_cast<float>(rooms_.size());
    const float content_w =
        n > 0.0f ? 2 * kChipPadX + n * kChipW + std::max(0.0f, n - 1.0f) * kChipGap
                 : kChipW + 2 * kChipPadX;
    const float cw = std::min(std::min(content_w, kCardW),
                              std::max(0.0f, bounds.w - 2 * kCardMargin));
    const float ch = kStripH;
    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    // Bias slightly above centre, matching QuickSwitcher's card placement.
    float cy = bounds.y + (bounds.h - ch) * 0.38f;
    cy = std::max(cy, bounds.y + kCardMargin);

    card_rect_ = {cx, cy, cw, ch};
}

void MruSwitcher::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
    {
        return;
    }

    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));

    ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f,
                                   ctx.theme.palette.popup_border, 1.0f);

    chips_.clear();

    RoomChipStripStyle style{};
    style.chip_w = kChipW;
    style.chip_gap = kChipGap;
    style.avatar_size = kAvatar;
    style.pad_x = 12.0f;
    style.top_pad = (kStripH - (kAvatar + 4.0f + 14.0f)) * 0.5f;
    style.highlight_index = selected_;
    style.highlight_fill = ctx.theme.palette.sidebar_selected;
    style.highlight_border = true;
    style.highlight_border_color = ctx.theme.palette.accent;

    paint_room_chips(ctx, card_rect_, rooms_, avatar_provider_,
                     on_room_avatar_needed, style, chips_);
}

bool MruSwitcher::on_pointer_down(tk::Point local)
{
    if (!is_open_)
    {
        return false;
    }
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    auto contains = [](const tk::Rect& r, tk::Point p)
    {
        return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
    };

    pressed_chip_ = -1;
    for (int i = 0; i < static_cast<int>(chips_.size()); ++i)
    {
        if (contains(chips_[static_cast<std::size_t>(i)].first, world))
        {
            pressed_chip_ = i;
            press_outside_ = false;
            return true;
        }
    }

    press_outside_ = !contains(card_rect_, world);
    return true;
}

void MruSwitcher::on_pointer_up(tk::Point local, bool inside_self)
{
    if (pressed_chip_ >= 0)
    {
        const int chip = pressed_chip_;
        pressed_chip_ = -1;
        const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
        if (chip < static_cast<int>(chips_.size()))
        {
            const auto& [rect, room_id] = chips_[static_cast<std::size_t>(chip)];
            const bool on_chip = world.x >= rect.x && world.x < rect.x + rect.w &&
                                 world.y >= rect.y && world.y < rect.y + rect.h;
            if (on_chip)
            {
                selected_ = chip;
                commit();
            }
        }
        return;
    }
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
        {
            cancel();
        }
    }
}

} // namespace tesseract::views
