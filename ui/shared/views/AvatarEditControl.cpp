#include "AvatarEditControl.h"

#include "tk/widget.h"
#include "views/media_utils.h"

namespace tesseract::views
{

namespace
{
constexpr float kRemoveChipR    = 9.0f;
constexpr float kXChipTolerance = kRemoveChipR + 4.0f;
constexpr float kAvatarEditErrorGap       = 4.0f;
} // namespace

void AvatarEditControl::set_geometry(tk::Point centre, float diameter)
{
    centre_   = centre;
    diameter_ = diameter;
}

void AvatarEditControl::set_avatar_url(std::string mxc_url)
{
    avatar_url_ = std::move(mxc_url);
    if (avatar_url_.empty())
        local_preview_.reset();
}

void AvatarEditControl::set_image_provider(ImageProvider provider)
{
    image_provider_ = std::move(provider);
}

void AvatarEditControl::set_local_preview(std::shared_ptr<tk::Image> image)
{
    local_preview_ = std::move(image);
}

void AvatarEditControl::set_editable(bool editable)
{
    editable_ = editable;
}

void AvatarEditControl::set_busy(bool busy)
{
    busy_ = busy;
    if (busy)
    {
        error_.clear();
        error_layout_.reset();
    }
}

void AvatarEditControl::set_error(std::string error)
{
    error_ = std::move(error);
    error_layout_.reset();
}

AvatarEditControl::HitZone AvatarEditControl::hit_test(tk::Point local) const
{
    if (!editable_ || busy_)
        return HitZone::None;
    if (!avatar_url_.empty() || local_preview_)
    {
        const float radius = diameter_ * 0.5f;
        const float cx     = centre_.x + radius - kRemoveChipR;
        const float cy     = centre_.y - radius + kRemoveChipR;
        const float dx     = local.x - cx;
        const float dy     = local.y - cy;
        if ((dx * dx + dy * dy) <= (kXChipTolerance * kXChipTolerance))
            return HitZone::RemoveChip;
    }
    const float dx = local.x - centre_.x;
    const float dy = local.y - centre_.y;
    const float radius = diameter_ * 0.5f;
    if ((dx * dx + dy * dy) <= (radius * radius))
        return HitZone::Disc;
    return HitZone::None;
}

bool AvatarEditControl::on_pointer_move(tk::Point local)
{
    if (!editable_)
        return false;
    const bool was = hovered_;
    const float dx = local.x - centre_.x;
    const float dy = local.y - centre_.y;
    const float radius = diameter_ * 0.5f;
    hovered_ = (dx * dx + dy * dy) <= (radius * radius);
    return hovered_ != was;
}

void AvatarEditControl::on_pointer_leave()
{
    hovered_ = false;
}

void AvatarEditControl::paint(tk::PaintCtx& ctx, tk::Point world_origin,
                              std::string_view initials_source) const
{
    const auto& pal = ctx.theme.palette;
    const float radius = diameter_ * 0.5f;
    const tk::Point centre{world_origin.x + centre_.x, world_origin.y + centre_.y};

    const tk::Image* img = local_preview_
                              ? local_preview_.get()
                              : (image_provider_ && !avatar_url_.empty())
                                  ? image_provider_(avatar_url_)
                                  : nullptr;
    draw_avatar(ctx.canvas, img, centre, diameter_, initials_source,
               pal.avatar_initials_bg, pal.avatar_initials_text);

    if (!editable_)
        return;

    const tk::Rect disc_rect{centre.x - radius, centre.y - radius, diameter_,
                             diameter_};

    if (busy_)
    {
        ctx.canvas.push_clip_rounded_rect(disc_rect, radius);
        ctx.canvas.fill_rect(disc_rect, tk::Color::rgba(0, 0, 0, 160));
        ctx.canvas.pop_clip();
        tk::TextStyle st;
        st.role = tk::FontRole::Title;
        auto lay = ctx.factory.build_text("\xe2\x80\xa6", st);
        if (lay)
        {
            const tk::Size sz = lay->measure();
            ctx.canvas.draw_text(*lay,
                                 {centre.x - sz.w * 0.5f, centre.y - sz.h * 0.5f},
                                 tk::Color::rgb(0xffffff));
        }
    }
    else if (hovered_)
    {
        ctx.canvas.push_clip_rounded_rect(disc_rect, radius);
        ctx.canvas.fill_rect(disc_rect, tk::Color::rgba(0, 0, 0, 128));
        ctx.canvas.pop_clip();
        tk::TextStyle st;
        st.role = tk::FontRole::Title;
        auto lay = ctx.factory.build_text("+", st);
        if (lay)
        {
            const tk::Size sz = lay->measure();
            ctx.canvas.draw_text(*lay,
                                 {centre.x - sz.w * 0.5f, centre.y - sz.h * 0.5f},
                                 tk::Color::rgb(0xffffff));
        }

        if (!avatar_url_.empty() || local_preview_)
        {
            const float cx = centre.x + radius - kRemoveChipR;
            const float cy = centre.y - radius + kRemoveChipR;
            ctx.canvas.fill_rounded_rect(
                {cx - kRemoveChipR, cy - kRemoveChipR, kRemoveChipR * 2.0f,
                 kRemoveChipR * 2.0f},
                kRemoveChipR, tk::Color::rgba(40, 40, 40, 220));
            tk::TextStyle xs;
            xs.role = tk::FontRole::Small;
            auto xlay = ctx.factory.build_text("\xc3\x97", xs);
            if (xlay)
            {
                const tk::Size xsz = xlay->measure();
                ctx.canvas.draw_text(*xlay,
                                     {cx - xsz.w * 0.5f, cy - xsz.h * 0.5f},
                                     tk::Color::rgb(0xffffff));
            }
        }
    }

    if (!error_.empty())
    {
        if (!error_layout_)
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.max_width = diameter_;
            error_layout_ = ctx.factory.build_text(error_, st);
        }
        if (error_layout_)
        {
            ctx.canvas.draw_text(*error_layout_,
                                 {disc_rect.x, disc_rect.y + diameter_ + kAvatarEditErrorGap},
                                 tk::Color::rgb(0xcc3333));
        }
    }
}

} // namespace tesseract::views
