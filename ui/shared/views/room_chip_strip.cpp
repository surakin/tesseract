#include "room_chip_strip.h"

#include "media_utils.h"
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
            room.name.empty() ? std::string("Unnamed") : room.name, ls);
        if (lo)
        {
            ctx.canvas.draw_text(*lo,
                                 {chip.x, chip_y + style.avatar_size + kLabelGap},
                                 pal.text_primary);
        }

        out_chips.push_back({chip, room.id});
        x += style.chip_w + style.chip_gap;
    }
}

} // namespace tesseract::views
