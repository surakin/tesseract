#include "room_chip_strip.h"

#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kCaptionH = 16.0f;
constexpr float kLabelGap = 4.0f;
constexpr float kLabelH = 14.0f;
} // namespace

void paint_room_chips(tk::PaintCtx& ctx, tk::Rect strip,
                      const std::vector<tesseract::RoomInfo>& rooms,
                      const ChipAvatarProvider& avatar_provider,
                      const ChipAvatarNeeded& on_avatar_needed,
                      const RoomChipStripStyle& style,
                      std::vector<std::pair<tk::Rect, std::string>>& out_chips)
{
    const auto& pal = ctx.theme.palette;

    float chip_y = strip.y + style.top_pad;
    if (!style.caption.empty())
    {
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Small;
        auto cap = ctx.factory.build_text(style.caption, cs);
        if (cap)
        {
            ctx.canvas.draw_text(*cap, {strip.x + style.pad_x, chip_y},
                                 pal.text_muted);
        }
        chip_y += kCaptionH;
    }

    const float chip_h = style.avatar_size + kLabelGap + kLabelH;
    const float avail = strip.w - 2 * style.pad_x;
    const int max_fit =
        avail > 0.0f ? static_cast<int>((avail + style.chip_gap) /
                                        (style.chip_w + style.chip_gap))
                     : 0;
    const int count = std::min(static_cast<int>(rooms.size()), std::max(0, max_fit));

    float x = strip.x + style.pad_x;
    for (int i = 0; i < count; ++i)
    {
        const auto& room = rooms[static_cast<std::size_t>(i)];
        const tk::Rect chip{x, chip_y, style.chip_w, chip_h};
        const bool highlighted = style.highlight_index == i;

        if (highlighted)
        {
            ctx.canvas.fill_rounded_rect(chip, 8.0f, style.highlight_fill);
            if (style.highlight_border)
            {
                ctx.canvas.stroke_rounded_rect(chip, 8.0f,
                                               style.highlight_border_color, 1.5f);
            }
        }

        const float acx = chip.x + chip.w * 0.5f;
        const float acy = chip_y + style.avatar_size * 0.5f;
        const tk::Image* avatar = nullptr;
        const std::string& mxc = room.effective_avatar_url();
        if (avatar_provider && !mxc.empty())
        {
            avatar = avatar_provider(mxc);
            if (!avatar && on_avatar_needed)
                on_avatar_needed(room);
        }
        draw_avatar(ctx.canvas, avatar, {acx, acy}, style.avatar_size, room.name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        tk::TextStyle ls{};
        ls.role = tk::FontRole::Small;
        ls.halign = tk::TextHAlign::Center;
        ls.trim = tk::TextTrim::Ellipsis;
        ls.max_width = chip.w;
        auto lo = ctx.factory.build_text(
            room.name.empty() ? tk::tr("Unnamed") : room.name, ls);
        if (lo)
        {
            ctx.canvas.draw_text(*lo,
                                 {chip.x, chip_y + style.avatar_size + kLabelGap},
                                 highlighted ? tk::on_selected(ctx.theme, pal.text_primary)
                                             : pal.text_primary);
        }

        out_chips.push_back({chip, room.id});
        x += style.chip_w + style.chip_gap;
    }
}

void RoomChipAreas::paint(tk::PaintCtx& ctx)
{
    for (tk::HoverArea* a : areas_)
        if (a->bounds().w > 0.0f)
            a->paint(ctx);
}

void RoomChipAreas::park()
{
    for (tk::HoverArea* a : areas_)
        a->set_rect({});
}

void RoomChipAreas::sync(tk::Widget& owner,
                         const std::vector<std::pair<tk::Rect, std::string>>& chips,
                         OnChip on_chip)
{
    on_chip_ = std::move(on_chip);
    room_ids_.clear();
    for (const auto& c : chips)
        room_ids_.push_back(c.second);

    while (areas_.size() < chips.size())
    {
        const std::size_t idx = areas_.size();
        auto* a = owner.add_child(
            tk::create_widget<tk::HoverArea>(&owner, tk::HoverStyle::Fill, 8.0f));
        a->set_on_click(
            [this, idx]
            {
                // Copy first: the handler closes the overlay, which may
                // rebuild this strip.
                if (idx >= room_ids_.size() || !on_chip_)
                    return;
                const std::string id = room_ids_[idx];
                auto cb = on_chip_;
                cb(idx, id);
            });
        areas_.push_back(a);
    }
    for (std::size_t i = 0; i < areas_.size(); ++i)
        areas_[i]->set_rect(i < chips.size() ? chips[i].first : tk::Rect{});
}

} // namespace tesseract::views
