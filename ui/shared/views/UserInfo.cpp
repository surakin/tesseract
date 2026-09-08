#include "UserInfo.h"

#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/media_utils.h"

#include <tesseract/visual.h>

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Visual constants. Sized to fit inside the 64 px sidebar user-strip slot
// with the default avatar size and all three text lines present.
constexpr float kUserInfoPadX = 12.0f;
constexpr float kUserInfoPadY = 8.0f;
constexpr float kUserInfoAvatarTextGap = 10.0f;
constexpr float kUserInfoLineGap = 2.0f;
constexpr float kHoverRadius = tesseract::visual::kRadiusSM;
// Matches RoomListView's active-room left bar so the active-account row
// reads the same way an active room does.
constexpr float kActiveBarW = 3.0f;

// MSC4426 status line's approximate slot height, used only by measure() to
// estimate the row's natural height before any real text layout exists.
// paint() computes the real height itself once it has built the emoji/text
// layouts (see its own status-line comment) rather than relying on this
// constant or on any backend's TextVAlign::Center — that request-a-box-
// and-let-the-backend-centre-it approach turned out to behave differently
// per backend (Qt centres within the box; CoreText on macOS ignores it and
// always draws at the box's literal top, and a too-tight box can also make
// CoreText drop a line that doesn't fit it entirely). Centring by hand from
// each layout's own measure() is simple, has no such backend-specific
// surprises, and is what paint() does now.
constexpr float kUserInfoStatusRowH = 20.0f;

// Slightly transparent ink for the Matrix ID line. The palette has a
// dedicated `text_muted` token used by timestamps; we reuse it here.
tk::Color id_colour(const tk::Theme& theme)
{
    return theme.palette.text_muted;
}

} // namespace

// ---------------------------------------------------------------------------

UserInfo::UserInfo() = default;

void UserInfo::set_display_name(std::string name)
{
    if (display_name_ == name)
    {
        return;
    }
    display_name_ = std::move(name);
    invalidate_text();
}

void UserInfo::set_user_id(std::string uid)
{
    if (user_id_ == uid)
    {
        return;
    }
    user_id_ = std::move(uid);
    invalidate_text();
}

void UserInfo::set_avatar_url(std::string mxc_url)
{
    avatar_url_ = std::move(mxc_url);
}

void UserInfo::set_status(std::string emoji, std::string text)
{
    if (status_emoji_ == emoji && status_text_ == text)
    {
        return;
    }
    status_emoji_ = std::move(emoji);
    status_text_ = std::move(text);
    status_emoji_layout_.reset();
    status_layout_.reset();
}

void UserInfo::set_status_line_enabled(bool enabled)
{
    if (status_line_enabled_ == enabled)
    {
        return;
    }
    status_line_enabled_ = enabled;
    status_emoji_layout_.reset();
    status_layout_.reset();
}

void UserInfo::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void UserInfo::set_active_indicator(bool on)
{
    active_indicator_ = on;
}

void UserInfo::set_notification_dot(bool on)
{
    notification_dot_ = on;
}

void UserInfo::invalidate_text()
{
    name_layout_.reset();
    uid_layout_.reset();
    status_emoji_layout_.reset();
    status_layout_.reset();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size UserInfo::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Width: take whatever the parent offers — the user-strip and the
    // picker rows both want to stretch to fill the column.
    const float w = constraints.w > 0 ? constraints.w : 0;

    // Height: max(avatar, name + (id if shown)) + 2 × pad.
    // We don't have a text layout here (factory not threaded into measure
    // for plain widgets) so we approximate the text-column height from
    // FontRole metric ranges. The numbers below match what the four
    // backends actually emit for SidebarName (≈18 px) and SidebarPreview
    // (≈14 px); paint() doesn't depend on these — they just feed the
    // natural-height calculation.
    constexpr float kNameH = 18.0f;
    constexpr float kIdH = 14.0f;

    float text_col_h = kNameH + kUserInfoLineGap + kIdH;
    if (status_line_enabled_)
        text_col_h += kUserInfoLineGap + kUserInfoStatusRowH;
    const float h = std::max(avatar_size_, text_col_h) + 2 * kUserInfoPadY;
    return {w, h};
}

void UserInfo::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void UserInfo::paint(tk::PaintCtx& ctx)
{
    const auto& theme = ctx.theme;

    // Row backdrop. The active-account row (AccountPicker only) gets the
    // same treatment RoomListView gives the active room — a tinted fill plus
    // a left accent bar — so it reads as "selected", distinct from the
    // small unread dot on the avatar corner. Hover/press stay subtle and
    // only apply when not the active row.
    if (active_indicator_ || hovered_ || pressed_)
    {
        tk::Rect r = bounds_;
        r.x += 4;
        r.y += 2;
        r.w -= 8;
        r.h -= 4;
        const tk::Color bg = active_indicator_ ? theme.palette.sidebar_selected
                              : pressed_        ? theme.palette.subtle_pressed
                                                : theme.palette.subtle_hover;
        ctx.canvas.fill_rounded_rect(r, kHoverRadius, bg);
        if (active_indicator_)
        {
            ctx.canvas.push_clip_rounded_rect(r, kHoverRadius);
            ctx.canvas.fill_rect({r.x, r.y, kActiveBarW, r.h}, theme.palette.accent);
            ctx.canvas.pop_clip();
        }
    }

    // -------- Avatar (left column) --------
    const tk::Point avatar_centre{
        bounds_.x + kUserInfoPadX + avatar_size_ * 0.5f,
        bounds_.y + bounds_.h * 0.5f,
    };

    const tk::Image* img = (image_provider_ && !avatar_url_.empty())
                               ? image_provider_(avatar_url_)
                               : nullptr;
    if (!img && !avatar_url_.empty() && on_avatar_needed)
    {
        on_avatar_needed(avatar_url_);
    }

    // Initials fallback. Prefer the display name for the glyph; fall back to
    // the localpart of the Matrix ID when the name is empty.
    std::string_view name_source;
    if (!display_name_.empty())
    {
        name_source = display_name_;
    }
    else if (!user_id_.empty())
    {
        name_source = user_id_;
        // Strip the leading '@' so the disc shows "A", not "@".
        if (name_source.front() == '@')
        {
            name_source.remove_prefix(1);
        }
    }
    draw_avatar(ctx.canvas, img, avatar_centre, avatar_size_, name_source,
                theme.palette.avatar_initials_bg,
                theme.palette.avatar_initials_text);

    // -------- Notification dot (avatar corner) --------
    // Ring first (matches the row's current backdrop so the dot "punches
    // out" of the avatar edge, mirroring RoomListView's presence dot), then
    // the dot itself.
    if (notification_dot_)
    {
        constexpr float kDotD = 8.0f;
        constexpr float kRing = 2.0f;
        const float outer_d = kDotD + kRing * 2.0f;
        const float dot_cx = avatar_centre.x + avatar_size_ * 0.5f;
        const float dot_cy = avatar_centre.y + avatar_size_ * 0.5f;
        const tk::Color ring_col = pressed_ ? theme.palette.subtle_pressed
                                   : hovered_ ? theme.palette.subtle_hover
                                              : theme.palette.sidebar_bg;
        ctx.canvas.fill_rounded_rect(
            {dot_cx - outer_d * 0.5f, dot_cy - outer_d * 0.5f, outer_d, outer_d},
            outer_d * 0.5f, ring_col);
        ctx.canvas.fill_rounded_rect(
            {dot_cx - kDotD * 0.5f, dot_cy - kDotD * 0.5f, kDotD, kDotD},
            kDotD * 0.5f, theme.palette.unread_bg);
    }

    // -------- Text column --------
    const float text_x = bounds_.x + kUserInfoPadX + avatar_size_ + kUserInfoAvatarTextGap;
    const float text_w =
        std::max(0.0f, bounds_.x + bounds_.w - kUserInfoPadX - text_x);

    // (Re)build text layouts on demand. The factory is bound to the
    // backend, so layouts must be rebuilt whenever the text changes — the
    // invalidate_text() helper drops them on every setter.
    if (!name_layout_ && !display_name_.empty())
    {
        tk::TextStyle st;
        st.role = tk::FontRole::SidebarName;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        name_layout_ = ctx.factory.build_text(display_name_, st);
    }
    if (!uid_layout_ && !user_id_.empty())
    {
        tk::TextStyle st;
        st.role = tk::FontRole::SidebarPreview;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        uid_layout_ = ctx.factory.build_text(user_id_, st);
    }

    // MSC4426 status line (sidebar strip only). "<emoji>  <text>" — either
    // part may be absent — or a "Click to set status" placeholder when both
    // are empty. Plain text, no linkification (MSC guidance). The emoji and
    // text are two separate layouts (the emoji renders a step larger), so
    // they're centred on each other by hand below, from their own
    // unconstrained measure() — no max_height/TextVAlign::Center: that
    // "hand the box to the backend" approach behaved differently per
    // backend (Qt centres within the box; CoreText on macOS always draws at
    // the box's literal top, ignoring the request, and can also drop a run
    // whose real line height exceeds too tight a box entirely).
    constexpr float kStatusEmojiGap = 5.0f;
    const bool status_is_placeholder =
        status_emoji_.empty() && status_text_.empty();
    if (status_line_enabled_ && !status_layout_ && !status_emoji_layout_)
    {
        tk::TextStyle st;
        st.role = tk::FontRole::SidebarPreview;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;

        if (status_is_placeholder)
        {
            status_layout_ =
                ctx.factory.build_text(tk::tr("Click to set status"), st);
        }
        else
        {
            float used = 0.0f;
            if (!status_emoji_.empty())
            {
                tk::TextStyle est = st;
                // Body, not InlineEmoji (~125% of *Body*, not of the smaller
                // SidebarPreview text below it — noticeably oversized next
                // to it): a modest, deliberate step up from SidebarPreview.
                est.role = tk::FontRole::Body;
                est.trim = tk::TextTrim::None;
                status_emoji_layout_ =
                    ctx.factory.build_text(status_emoji_, est);
                if (status_emoji_layout_)
                    used = status_emoji_layout_->measure().w + kStatusEmojiGap;
            }
            if (!status_text_.empty())
            {
                st.max_width = std::max(0.0f, text_w - used);
                status_layout_ = ctx.factory.build_text(status_text_, st);
            }
        }
    }

    // Vertically centre the text column inside the row.
    const tk::Size name_sz =
        name_layout_ ? name_layout_->measure() : tk::Size{};
    const tk::Size uid_sz = uid_layout_ ? uid_layout_->measure() : tk::Size{};
    const bool has_status_line =
        status_line_enabled_ && (status_layout_ || status_emoji_layout_);
    const tk::Size status_emoji_sz =
        status_emoji_layout_ ? status_emoji_layout_->measure() : tk::Size{};
    const tk::Size status_text_sz =
        status_layout_ ? status_layout_->measure() : tk::Size{};
    const float status_line_h = std::max(status_emoji_sz.h, status_text_sz.h);
    float col_h =
        name_sz.h + (uid_sz.h > 0 ? kUserInfoLineGap + uid_sz.h : 0);
    if (has_status_line)
        col_h += kUserInfoLineGap + status_line_h;
    const float col_top = bounds_.y + (bounds_.h - col_h) * 0.5f;

    if (name_layout_)
    {
        ctx.canvas.draw_text(*name_layout_, {text_x, col_top},
                             theme.palette.text_primary);
    }
    else if (display_name_.empty() && !user_id_.empty())
    {
        // No display name: surface the user_id as the primary line so the
        // row isn't a lone empty avatar.
        tk::TextStyle st;
        st.role = tk::FontRole::SidebarName;
        st.halign = tk::TextHAlign::Leading;
        st.valign = tk::TextVAlign::Top;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lay = ctx.factory.build_text(user_id_, st);
        // build_text can return nullptr on backend/font error or OOM; the
        // member-layout draws above guard their results, this local did not.
        if (lay)
            ctx.canvas.draw_text(*lay, {text_x, col_top},
                                 theme.palette.text_primary);
    }

    const float uid_y = col_top + name_sz.h + kUserInfoLineGap;
    if (uid_layout_)
    {
        ctx.canvas.draw_text(*uid_layout_, {text_x, uid_y}, id_colour(theme));
    }

    // Status line, and its world-space rect for on_pointer_up() hit-testing.
    status_rect_ = {};
    if (has_status_line)
    {
        const float status_y = uid_y + uid_sz.h + kUserInfoLineGap;
        // Centre each run on the other by hand: offset its top by half the
        // difference between its own height and the taller of the two, so
        // both verticals meet at status_y + status_line_h / 2.
        float x = text_x;
        if (status_emoji_layout_)
        {
            const float y = status_y + (status_line_h - status_emoji_sz.h) * 0.5f;
            ctx.canvas.draw_text(*status_emoji_layout_, {x, y},
                                 theme.palette.text_secondary);
            x += status_emoji_sz.w + kStatusEmojiGap;
        }
        if (status_layout_)
        {
            const float y = status_y + (status_line_h - status_text_sz.h) * 0.5f;
            // Placeholder reads as disabled text (text_muted); a real status
            // gets the slightly more present text_secondary.
            ctx.canvas.draw_text(*status_layout_, {x, y},
                                 status_is_placeholder ? theme.palette.text_muted
                                                       : theme.palette.text_secondary);
        }
        // Hit target: the whole text column from the status line down to the
        // bottom of the row, so a click on that line (or the padding just
        // below it) opens Settings rather than the account picker.
        status_rect_ = {text_x, status_y - kUserInfoLineGap * 0.5f, text_w,
                        std::max(status_line_h + kUserInfoLineGap,
                                 bounds_.y + bounds_.h - status_y)};
    }
}

// ---------------------------------------------------------------------------
// Pointer handling
// ---------------------------------------------------------------------------

bool UserInfo::on_pointer_down(tk::Point)
{
    pressed_ = true;
    return true;
}

void UserInfo::on_pointer_up(tk::Point local, bool inside_self)
{
    pressed_ = false;
    if (!inside_self)
    {
        return;
    }
    // Convert back to world coords so the host can anchor popovers.
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    // A release on the status line routes to on_status_clicked (opens
    // Settings → Account), never to on_primary (which opens the account
    // picker). status_rect_ is world-space, cached in the last paint().
    if (status_line_enabled_ && on_status_clicked &&
        world.x >= status_rect_.x && world.x < status_rect_.x + status_rect_.w &&
        world.y >= status_rect_.y && world.y < status_rect_.y + status_rect_.h)
    {
        on_status_clicked();
        return;
    }

    if (on_primary)
    {
        on_primary(world);
    }
}

bool UserInfo::on_pointer_move(tk::Point)
{
    bool prev = hovered_;
    hovered_ = true;
    return !prev;
}

void UserInfo::on_pointer_leave()
{
    hovered_ = false;
    pressed_ = false;
}

} // namespace tesseract::views
