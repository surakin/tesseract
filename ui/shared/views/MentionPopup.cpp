#include "views/MentionPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/media_utils.h"

namespace tesseract::views
{

void MentionPopup::set_candidates(std::vector<MentionCandidate> c)
{
    candidates_ = std::move(c);
    selected_index_ = candidates_.empty() ? -1 : 0;
    reset_transient_state_();
}

void MentionPopup::paint_row(tk::PaintCtx& ctx, const tk::Rect& row,
                             size_t index, bool /*selected*/, bool /*hovered*/)
{
    const auto& pal = ctx.theme.palette;
    const auto& c   = candidates_[index];

    // Avatar: the member's image when available, else an initials disc
    // (accent disc for @room).
    constexpr float kAvatar = 28.0f;
    tk::Rect av{row.x + 6.0f, row.y + (kRowHeight - kAvatar) * 0.5f, kAvatar,
                kAvatar};
    const tk::Image* avatar_img =
        (!c.is_room && image_provider_ && !c.avatar_url.empty())
            ? image_provider_(c.avatar_url)
            : nullptr;
    tk::Point center{av.x + kAvatar * 0.5f, av.y + kAvatar * 0.5f};
    if (c.is_room && !avatar_img)
    {
        // Accent disc for @room.
        draw_avatar(ctx.canvas, nullptr, center, kAvatar, "#", pal.accent,
                    pal.text_on_accent);
    }
    else
    {
        const std::string& nm =
            !c.display_name.empty() ? c.display_name : c.user_id;
        draw_avatar(ctx.canvas, avatar_img, center, kAvatar, nm,
                    pal.avatar_initials_bg, pal.avatar_initials_text);
    }

    float text_x = av.x + av.w + 10.0f;

    // Primary: display name.
    std::string primary = c.is_room ? std::string("@room") : c.display_name;
    tk::TextStyle pst{};
    pst.role = tk::FontRole::Body;
    pst.halign = tk::TextHAlign::Leading;
    pst.valign = tk::TextVAlign::Top;
    float primary_w = 0.0f;
    auto pl = ctx.factory.build_text(primary, pst);
    if (pl)
    {
        tk::Size psz = pl->measure();
        primary_w = psz.w;
        float ly = row.y + (kRowHeight - psz.h) * 0.5f;
        ctx.canvas.draw_text(*pl, {text_x, ly}, pal.text_primary);
    }

    // Secondary (muted): user id, or a hint for @room.
    std::string secondary =
        c.is_room ? std::string("Notify the whole room") : c.user_id;
    if (!secondary.empty())
    {
        tk::TextStyle sst{};
        sst.role = tk::FontRole::Small;
        sst.halign = tk::TextHAlign::Leading;
        sst.valign = tk::TextVAlign::Top;
        auto sl = ctx.factory.build_text(secondary, sst);
        if (sl)
        {
            tk::Size ssz = sl->measure();
            float sx = text_x + primary_w + 8.0f;
            // Keep the muted id inside the popup; drop it if no room.
            if (sx + ssz.w <= bounds_.x + bounds_.w - 6.0f)
            {
                float ly = row.y + (kRowHeight - ssz.h) * 0.5f;
                ctx.canvas.draw_text(*sl, {sx, ly}, pal.text_muted);
            }
        }
    }
}

} // namespace tesseract::views
