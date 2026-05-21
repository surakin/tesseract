#include "UserProfilePanel.h"
#include "media_utils.h"

#include "tk/theme.h"

namespace tesseract::views
{

// ── constructor ───────────────────────────────────────────────────────────

UserProfilePanel::UserProfilePanel()
{
    close_btn_ = add_child(
        std::make_unique<tk::Button>("\xC3\x97", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    dm_btn_ = add_child(
        std::make_unique<tk::Button>("Message", std::function<void()>{},
                                     tk::Button::Variant::Primary));
    ignore_btn_ = add_child(
        std::make_unique<tk::Button>("Ignore", std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    close_btn_->set_on_click([this]() { if (on_close) on_close(); });
    dm_btn_->set_on_click([this]() { if (on_open_dm) on_open_dm(user_id_); });
    ignore_btn_->set_on_click([this]() { if (on_ignore) on_ignore(user_id_); });

    // Closed-by-default overlay. Tie widget visibility to the open state so
    // the Widget tree's hit-test walks past us entirely when closed — the
    // dm_btn_ and ignore_btn_ children are otherwise visible-by-default
    // (Widget contract) and would capture clicks at their last-arranged rects
    // even though paint() draws nothing.
    set_visible(false);
}

// ── public API ────────────────────────────────────────────────────────────

void UserProfilePanel::open(std::string user_id, std::string display_name,
                             std::string avatar_url)
{
    const bool was_open = open_;
    user_id_      = std::move(user_id);
    display_name_ = std::move(display_name);
    avatar_url_   = std::move(avatar_url);
    open_         = true;
    set_visible(true);
    name_layout_.reset();
    uid_layout_.reset();
    // Tells the shell to re-query rect accessors so the compose textarea +
    // room-search NativeTextField overlays hide while the panel is up.
    if (!was_open && on_layout_changed) on_layout_changed();
}

void UserProfilePanel::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (was_open && on_layout_changed) on_layout_changed();
}

void UserProfilePanel::set_avatar_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size UserProfilePanel::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void UserProfilePanel::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);
    backdrop_rect_ = bounds;

    // Card height: header + avatar + spacing + name row + uid row + buttons
    constexpr float kNameH    = 20.0f; // estimated single-line title height
    constexpr float kUidH     = 16.0f; // estimated small-font line height
    const float card_h = kHeaderH
                       + kAvatarD + kPadY
                       + kNameH   + kPadY * 0.5f
                       + kUidH    + kPadY
                       + kButtonH + kPadY * 0.5f
                       + kButtonH + kPadY;

    // Centre the card in the available space.
    const float card_x = bounds.x + (bounds.w - kCardW) * 0.5f;
    const float card_y = bounds.y + (bounds.h - card_h) * 0.5f;
    card_rect_ = {card_x, card_y, kCardW, card_h};

    // Close button: 32×32 anchored to top-right of card with 4px margin.
    constexpr float kCloseSz = 32.0f;
    if (close_btn_)
    {
        close_btn_->arrange(lc, {card_rect_.x + card_rect_.w - kCloseSz - 4.0f,
                                  card_rect_.y + 4.0f, kCloseSz, kCloseSz});
    }

    // Avatar circle centred horizontally below the header row.
    const float av_x = card_rect_.x + (kCardW - kAvatarD) * 0.5f;
    const float av_y = card_rect_.y + kHeaderH;
    avatar_rect_     = {av_x, av_y, kAvatarD, kAvatarD};

    // Text rows (painted directly — no Label children, just TextLayouts).
    // Invalidate caches so paint() rebuilds them at the correct width.
    name_layout_.reset();
    uid_layout_.reset();

    // Buttons: full inner width, stacked below text rows.
    const float btn_x = card_rect_.x + kPadX;
    const float btn_w = kCardW - kPadX * 2.0f;
    const float btn_y_dm = av_y + kAvatarD + kPadY
                         + kNameH + kPadY * 0.5f
                         + kUidH  + kPadY;
    const float btn_y_ignore = btn_y_dm + kButtonH + kPadY * 0.5f;

    if (dm_btn_)
    {
        dm_btn_->arrange(lc, {btn_x, btn_y_dm, btn_w, kButtonH});
    }
    if (ignore_btn_)
    {
        ignore_btn_->arrange(lc, {btn_x, btn_y_ignore, btn_w, kButtonH});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void UserProfilePanel::paint(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }

    auto& cv         = ctx.canvas;
    const auto& pal  = ctx.theme.palette;

    // Semi-transparent backdrop over the whole surface.
    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 100});

    // Card background + border.
    constexpr float kCornerR = 8.0f;
    cv.fill_rounded_rect(card_rect_, kCornerR, pal.bg);
    cv.stroke_rounded_rect(card_rect_, kCornerR, pal.popup_border, 1.0f);

    // Avatar.
    const tk::Point av_centre{avatar_rect_.x + kAvatarD * 0.5f,
                               avatar_rect_.y + kAvatarD * 0.5f};
    const tk::Image* av_img = nullptr;
    if (image_provider_ && !avatar_url_.empty())
    {
        av_img = image_provider_(avatar_url_);
    }
    if (av_img)
    {
        cv.draw_circle_image(*av_img, av_centre, kAvatarD);
    }
    else
    {
        std::string_view disp =
            display_name_.empty() ? std::string_view("?")
                                  : std::string_view(display_name_);
        cv.draw_initials_circle(disp, av_centre, kAvatarD,
                                pal.accent, tk::Color{255, 255, 255, 255});
    }

    // Display name (Title, centred, ellipsis).
    constexpr float kNameH = 20.0f;
    constexpr float kUidH  = 16.0f;
    const float text_max_w = kCardW - kPadX * 2.0f;
    const float name_y     = avatar_rect_.y + kAvatarD + kPadY;

    if (!name_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        name_layout_ = ctx.factory.build_text(
            display_name_.empty() ? user_id_ : display_name_, st);
    }
    if (name_layout_)
    {
        const tk::Size sz = name_layout_->measure();
        const float tx    = card_rect_.x + (kCardW - sz.w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, name_y}, pal.text_primary);
    }

    // User ID (Small, centred, ellipsis, muted colour).
    const float uid_y = name_y + kNameH + kPadY * 0.5f;

    if (!uid_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        uid_layout_  = ctx.factory.build_text(user_id_, st);
    }
    if (uid_layout_)
    {
        const tk::Size sz = uid_layout_->measure();
        const float tx    = card_rect_.x + (kCardW - sz.w) * 0.5f;
        cv.draw_text(*uid_layout_, {tx, uid_y}, pal.text_muted);
    }

    // Child buttons.
    if (close_btn_)  close_btn_->paint(ctx);
    if (dm_btn_)     dm_btn_->paint(ctx);
    if (ignore_btn_) ignore_btn_->paint(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool UserProfilePanel::on_pointer_down(tk::Point local)
{
    if (!open_)
    {
        return false;
    }

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (rect_contains(card_rect_, w))
    {
        // Let the child dispatch (buttons) handle events inside the card.
        return false;
    }

    // Backdrop click: consume and remember for on_pointer_up.
    press_backdrop_ = true;
    return true;
}

void UserProfilePanel::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
    {
        return;
    }
    press_backdrop_ = false;

    if (inside_self)
    {
        const tk::Point w{local.x + bounds().x, local.y + bounds().y};
        if (!rect_contains(card_rect_, w))
        {
            if (on_close)
            {
                on_close();
            }
        }
    }
}

bool UserProfilePanel::on_pointer_move(tk::Point /*local*/)
{
    return false;
}

void UserProfilePanel::on_pointer_leave()
{
    press_backdrop_ = false;
}

} // namespace tesseract::views
