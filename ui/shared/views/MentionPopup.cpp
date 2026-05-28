#include "views/MentionPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"

namespace tesseract::views
{

void MentionPopup::set_candidates(std::vector<MentionCandidate> c)
{
    candidates_ = std::move(c);
    selected_index_ = candidates_.empty() ? -1 : 0;
    hovered_index_ = -1;
    pressed_index_ = -1;
}

void MentionPopup::set_selected_index(int index)
{
    selected_index_ = index;
}

tk::Size MentionPopup::measure(tk::LayoutCtx&, tk::Size)
{
    return {kWidth, kRowHeight * float(visible_rows())};
}

void MentionPopup::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

void MentionPopup::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    int n = visible_rows();

    ctx.canvas.fill_rect(bounds_, pal.bg);

    for (int i = 0; i < n; ++i)
    {
        tk::Rect row{bounds_.x, bounds_.y + float(i) * kRowHeight, bounds_.w,
                     kRowHeight};

        if (i == selected_index_)
        {
            ctx.canvas.fill_rect(row, pal.sidebar_selected);
        }
        else if (i == hovered_index_)
        {
            ctx.canvas.fill_rect(row, pal.subtle_hover);
        }

        const auto& c = candidates_[std::size_t(i)];

        // Avatar: the member's image when available, else an initials disc
        // (accent disc for @room).
        constexpr float kAvatar = 28.0f;
        tk::Rect av{row.x + 6.0f, row.y + (kRowHeight - kAvatar) * 0.5f,
                    kAvatar, kAvatar};
        const tk::Image* avatar_img =
            (!c.is_room && image_provider_ && !c.avatar_url.empty())
                ? image_provider_(c.avatar_url)
                : nullptr;
        tk::Point center{av.x + kAvatar * 0.5f, av.y + kAvatar * 0.5f};
        if (avatar_img)
        {
            ctx.canvas.draw_circle_image(*avatar_img, center, kAvatar);
        }
        else if (c.is_room)
        {
            ctx.canvas.draw_initials_circle("#", center, kAvatar, pal.accent,
                                            pal.text_on_accent);
        }
        else
        {
            const std::string& nm =
                !c.display_name.empty() ? c.display_name : c.user_id;
            ctx.canvas.draw_initials_circle(nm, center, kAvatar,
                                            pal.avatar_initials_bg,
                                            pal.avatar_initials_text);
        }

        float text_x = av.x + av.w + 10.0f;

        // Primary: display name.
        std::string primary =
            c.is_room ? std::string("@room") : c.display_name;
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

        if (i < n - 1)
        {
            tk::Rect sep{row.x, row.y + row.h - 1.0f, row.w, 1.0f};
            ctx.canvas.fill_rect(sep, pal.separator);
        }
    }

    ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f},
                         pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
        pal.separator);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, 1.0f, bounds_.h},
                         pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h},
        pal.separator);
}

bool MentionPopup::on_pointer_down(tk::Point local)
{
    pressed_index_ = row_at(local.y);
    return pressed_index_ >= 0;
}

void MentionPopup::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!inside_self)
    {
        pressed_index_ = -1;
        return;
    }
    int r = row_at(local.y);
    if (r >= 0 && r == pressed_index_ && r < (int)candidates_.size())
    {
        if (on_accepted)
        {
            on_accepted(candidates_[std::size_t(r)]);
        }
    }
    pressed_index_ = -1;
}

bool MentionPopup::on_pointer_move(tk::Point local)
{
    int prev = hovered_index_;
    hovered_index_ = row_at(local.y);
    return hovered_index_ != prev;
}

void MentionPopup::on_pointer_leave()
{
    hovered_index_ = -1;
}

bool MentionPopup::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    if (dy == 0.0f || visible_rows() == 0)
        return false;
    int delta = dy > 0.0f ? 1 : -1;
    int next  = std::clamp(selected_index_ + delta, 0, visible_rows() - 1);
    if (next == selected_index_)
        return false;
    selected_index_ = next;
    return true;
}

} // namespace tesseract::views
