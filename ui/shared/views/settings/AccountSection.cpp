#include "AccountSection.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Visual constants for the account section.
constexpr float kAvatarDiameter = 64.0f;
constexpr float kPadX = 24.0f;
constexpr float kPadY = 24.0f;
constexpr float kAvatarTextGap = 16.0f;
constexpr float kLineGap = 4.0f;

// Approximate text heights used in measure() — these match the heights the
// four backends emit for Title (≈20 px) and Body (≈17 px).
constexpr float kNameH = 20.0f;
constexpr float kIdH = 17.0f;

} // namespace

// ---------------------------------------------------------------------------

AccountSection::AccountSection() = default;

void AccountSection::set_display_name(std::string name)
{
    if (display_name_ == name)
    {
        return;
    }
    display_name_ = std::move(name);
    invalidate_text();
}

void AccountSection::set_user_id(std::string uid)
{
    if (user_id_ == uid)
    {
        return;
    }
    user_id_ = std::move(uid);
    invalidate_text();
}

void AccountSection::set_avatar_url(std::string mxc_url)
{
    avatar_url_ = std::move(mxc_url);
}

void AccountSection::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void AccountSection::invalidate_text()
{
    name_layout_.reset();
    uid_layout_.reset();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size AccountSection::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;

    // Height: max(avatar, name + id) + 2 × vertical padding.
    const float text_col_h = kNameH + kLineGap + kIdH;
    const float h = std::max(kAvatarDiameter, text_col_h) + 2.0f * kPadY;
    return {w, h};
}

void AccountSection::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void AccountSection::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // -------- Avatar --------------------------------------------------------
    const tk::Point avatar_centre{
        bounds_.x + kPadX + kAvatarDiameter * 0.5f,
        bounds_.y + kPadY + kAvatarDiameter * 0.5f,
    };

    const tk::Image* img = (image_provider_ && !avatar_url_.empty())
                               ? image_provider_(avatar_url_)
                               : nullptr;

    if (img)
    {
        ctx.canvas.draw_circle_image(*img, avatar_centre, kAvatarDiameter);
    }
    else
    {
        std::string_view name_source;
        if (!display_name_.empty())
        {
            name_source = display_name_;
        }
        else if (!user_id_.empty())
        {
            name_source = user_id_;
            if (name_source.front() == '@')
            {
                name_source.remove_prefix(1);
            }
        }
        ctx.canvas.draw_initials_circle(name_source, avatar_centre,
                                        kAvatarDiameter, pal.avatar_initials_bg,
                                        pal.avatar_initials_text);
    }

    // -------- Text column ---------------------------------------------------
    const float text_x = bounds_.x + kPadX + kAvatarDiameter + kAvatarTextGap;
    const float text_w = std::max(0.0f, bounds_.x + bounds_.w - kPadX - text_x);

    // Build text layouts lazily.
    if (!name_layout_ && !display_name_.empty())
    {
        tk::TextStyle st;
        st.role = tk::FontRole::Title;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        name_layout_ = ctx.factory.build_text(display_name_, st);
    }

    if (!uid_layout_ && !user_id_.empty())
    {
        tk::TextStyle st;
        st.role = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        uid_layout_ = ctx.factory.build_text(user_id_, st);
    }

    // Vertically centre the two-line text column within the avatar circle height,
    // both anchored at the top padding.
    const tk::Size name_sz =
        name_layout_ ? name_layout_->measure() : tk::Size{};
    const tk::Size uid_sz = uid_layout_ ? uid_layout_->measure() : tk::Size{};
    const float col_h = name_sz.h + (uid_sz.h > 0 ? kLineGap + uid_sz.h : 0);
    const float col_top =
        bounds_.y + kPadY + (kAvatarDiameter - col_h) * 0.5f;

    if (name_layout_)
    {
        ctx.canvas.draw_text(*name_layout_, {text_x, col_top},
                             pal.text_primary);
    }
    else if (display_name_.empty() && !user_id_.empty())
    {
        // No display name: fall back to user_id on the primary line.
        tk::TextStyle st;
        st.role = tk::FontRole::Title;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lay = ctx.factory.build_text(user_id_, st);
        ctx.canvas.draw_text(*lay, {text_x, col_top}, pal.text_primary);
    }

    if (uid_layout_)
    {
        ctx.canvas.draw_text(*uid_layout_,
                             {text_x, col_top + name_sz.h + kLineGap},
                             pal.text_secondary);
    }
}

} // namespace tesseract::views
