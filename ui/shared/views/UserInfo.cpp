#include "UserInfo.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views {

namespace {

// Visual constants. Matches the existing 48 px sidebar user-strip slot when
// the avatar is the default 32 px and both name + ID lines are present.
constexpr float kPadX           = 12.0f;
constexpr float kPadY           = 8.0f;
constexpr float kAvatarTextGap  = 10.0f;
constexpr float kLineGap        = 2.0f;
constexpr float kIndicatorSize  = 8.0f;
constexpr float kIndicatorPadR  = 10.0f;
constexpr float kHoverRadius    = 6.0f;

// Slightly transparent ink for the Matrix ID line. The palette has a
// dedicated `text_muted` token used by timestamps; we reuse it here.
tk::Color id_colour(const tk::Theme& theme) {
    return theme.palette.text_muted;
}

} // namespace

// ---------------------------------------------------------------------------

UserInfo::UserInfo() = default;

void UserInfo::set_display_name(std::string name) {
    if (display_name_ == name) return;
    display_name_ = std::move(name);
    invalidate_text();
}

void UserInfo::set_user_id(std::string uid) {
    if (user_id_ == uid) return;
    user_id_ = std::move(uid);
    invalidate_text();
}

void UserInfo::set_avatar_url(std::string mxc_url) {
    avatar_url_ = std::move(mxc_url);
}

void UserInfo::set_image_provider(ImageProvider p) {
    image_provider_ = std::move(p);
}

void UserInfo::set_active_indicator(bool on) {
    active_indicator_ = on;
}

void UserInfo::set_show_user_id(bool on) {
    if (show_user_id_ == on) return;
    show_user_id_ = on;
    invalidate_text();
}

void UserInfo::set_avatar_size(float d) {
    avatar_size_ = std::max(16.0f, d);
}

void UserInfo::invalidate_text() {
    name_layout_.reset();
    uid_layout_.reset();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size UserInfo::measure(tk::LayoutCtx&, tk::Size constraints) {
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
    constexpr float kIdH   = 14.0f;

    const float text_col_h = show_user_id_
        ? kNameH + kLineGap + kIdH
        : kNameH;
    const float h = std::max(avatar_size_, text_col_h) + 2 * kPadY;
    return { w, h };
}

void UserInfo::arrange(tk::LayoutCtx&, tk::Rect bounds) {
    bounds_ = bounds;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void UserInfo::paint(tk::PaintCtx& ctx) {
    const auto& theme = ctx.theme;

    // Hover/pressed backdrop — kept subtle; only paints when the pointer is
    // actually over the row, so a static, unhovered strip stays flat.
    if (hovered_ || pressed_) {
        tk::Rect r = bounds_;
        r.x += 4;
        r.y += 2;
        r.w -= 8;
        r.h -= 4;
        const tk::Color bg = pressed_ ? theme.palette.subtle_pressed
                                      : theme.palette.subtle_hover;
        ctx.canvas.fill_rounded_rect(r, kHoverRadius, bg);
    }

    // -------- Avatar (left column) --------
    const tk::Point avatar_centre {
        bounds_.x + kPadX + avatar_size_ * 0.5f,
        bounds_.y + bounds_.h * 0.5f,
    };

    const tk::Image* img = (image_provider_ && !avatar_url_.empty())
        ? image_provider_(avatar_url_)
        : nullptr;

    if (img) {
        ctx.canvas.draw_circle_image(*img, avatar_centre, avatar_size_);
    } else {
        // Initials fallback. Prefer the display name for the glyph; fall
        // back to the localpart of the Matrix ID when the name is empty.
        std::string_view name_source;
        if (!display_name_.empty()) {
            name_source = display_name_;
        } else if (!user_id_.empty()) {
            name_source = user_id_;
            // Strip the leading '@' so the disc shows "A", not "@".
            if (name_source.front() == '@') name_source.remove_prefix(1);
        }
        ctx.canvas.draw_initials_circle(
            name_source, avatar_centre, avatar_size_,
            theme.palette.avatar_initials_bg,
            theme.palette.avatar_initials_text);
    }

    // -------- Text column --------
    // Reserve space on the right for the active indicator so the text
    // layout's max_width clamps before it collides with the dot.
    const float right_reserved = active_indicator_
        ? kIndicatorSize + kIndicatorPadR + kPadX
        : kPadX;
    const float text_x = bounds_.x + kPadX + avatar_size_ + kAvatarTextGap;
    const float text_w = std::max(0.0f,
        bounds_.x + bounds_.w - right_reserved - text_x);

    // (Re)build text layouts on demand. The factory is bound to the
    // backend, so layouts must be rebuilt whenever the text changes — the
    // invalidate_text() helper drops them on every setter.
    if (!name_layout_ && !display_name_.empty()) {
        tk::TextStyle st;
        st.role      = tk::FontRole::SidebarName;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        name_layout_ = ctx.factory.build_text(display_name_, st);
    }
    if (!uid_layout_ && show_user_id_ && !user_id_.empty()) {
        tk::TextStyle st;
        st.role      = tk::FontRole::SidebarPreview;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        uid_layout_  = ctx.factory.build_text(user_id_, st);
    }

    // Vertically centre the text column inside the row.
    const tk::Size name_sz = name_layout_ ? name_layout_->measure() : tk::Size{};
    const tk::Size uid_sz  = uid_layout_  ? uid_layout_->measure()  : tk::Size{};
    const float col_h = show_user_id_
        ? name_sz.h + (uid_sz.h > 0 ? kLineGap + uid_sz.h : 0)
        : name_sz.h;
    const float col_top = bounds_.y + (bounds_.h - col_h) * 0.5f;

    if (name_layout_) {
        ctx.canvas.draw_text(*name_layout_, { text_x, col_top },
                             theme.palette.text_primary);
    } else if (display_name_.empty() && !user_id_.empty()) {
        // No display name: surface the user_id as the primary line so the
        // row isn't a lone empty avatar.
        tk::TextStyle st;
        st.role      = tk::FontRole::SidebarName;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        auto lay = ctx.factory.build_text(user_id_, st);
        ctx.canvas.draw_text(*lay, { text_x, col_top },
                             theme.palette.text_primary);
    }

    if (uid_layout_) {
        ctx.canvas.draw_text(*uid_layout_,
                             { text_x, col_top + name_sz.h + kLineGap },
                             id_colour(theme));
    }

    // -------- Active indicator (right) --------
    if (active_indicator_) {
        const tk::Rect dot {
            bounds_.x + bounds_.w - kIndicatorPadR - kIndicatorSize,
            bounds_.y + (bounds_.h - kIndicatorSize) * 0.5f,
            kIndicatorSize,
            kIndicatorSize,
        };
        ctx.canvas.fill_rounded_rect(dot, kIndicatorSize * 0.5f,
                                     theme.palette.accent);
    }
}

// ---------------------------------------------------------------------------
// Pointer handling
// ---------------------------------------------------------------------------

bool UserInfo::on_pointer_down(tk::Point) {
    pressed_ = true;
    return true;
}

void UserInfo::on_pointer_up(tk::Point local, bool inside_self) {
    pressed_ = false;
    if (inside_self && on_primary) {
        // Convert back to world coords so the host can anchor popovers.
        const tk::Point world {
            local.x + bounds_.x,
            local.y + bounds_.y,
        };
        on_primary(world);
    }
}

void UserInfo::on_pointer_move(tk::Point) {
    hovered_ = true;
}

void UserInfo::on_pointer_leave() {
    hovered_ = false;
    pressed_ = false;
}

} // namespace tesseract::views
