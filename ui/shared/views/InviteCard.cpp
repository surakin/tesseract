#include "InviteCard.h"

#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kRadius  = 8.0f;
constexpr float kBorderW = 1.0f;

// Estimate text-row heights (used in arrange before we have a real layout).
constexpr float kNameH       = 24.0f; // 18 pt bold — Title role
constexpr float kSecondaryH  = 18.0f; // 13 pt — Body role
constexpr float kInvitedByH  = 18.0f; // 12 pt — Small role

} // namespace

// ── constructor ───────────────────────────────────────────────────────────

InviteCard::InviteCard()
{
    auto accept = std::make_unique<tk::Button>(
        "Accept", std::function<void()>{}, tk::Button::Variant::Primary);
    accept->set_on_click([this]()
    {
        accept_btn_->set_enabled(false);
        if (on_accept)
            on_accept();
    });
    accept_btn_ = add_child(std::move(accept));

    auto decline = std::make_unique<tk::Button>(
        "Decline", std::function<void()>{}, tk::Button::Variant::Subtle);
    decline->set_on_click([this]() { if (on_decline) on_decline(); });
    decline_btn_ = add_child(std::move(decline));

    auto block = std::make_unique<tk::Button>(
        "Block", std::function<void()>{}, tk::Button::Variant::Destructive);
    block->set_on_click([this]() { if (on_block) on_block(); });
    block_btn_ = add_child(std::move(block));

    set_visible(false);
}

// ── public API ────────────────────────────────────────────────────────────

void InviteCard::set_invite(const tesseract::InviteInfo& info,
                             ImageProvider provider)
{
    invite_         = info;
    image_provider_ = std::move(provider);
    reset_layouts();

    // Re-enable the accept button in case it was disabled by a previous click.
    if (accept_btn_)
    {
        accept_btn_->set_enabled(true);
    }

    // Block button only appears in the DM variant.
    if (block_btn_)
    {
        block_btn_->set_visible(info.is_direct);
    }

    set_visible(true);
}

void InviteCard::clear()
{
    invite_.reset();
    image_provider_ = nullptr;
    reset_layouts();
    set_visible(false);
}

void InviteCard::reset_layouts()
{
    name_layout_.reset();
    secondary_layout_.reset();
    invited_by_layout_.reset();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size InviteCard::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill the available space; the content block is centred inside.
    return constraints;
}

void InviteCard::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    if (!invite_.has_value())
    {
        return;
    }

    const bool is_dm   = invite_->is_direct;
    const float av_d   = is_dm ? kAvatarD_DM : kAvatarD_GR;
    const float btn_w  = (kContentW - (is_dm ? kBtnGap * 2.0f : kBtnGap)) /
                         (is_dm ? 3.0f : 2.0f);

    // Estimate total content height for vertical centering.
    const float btn_row_h = kBtnH;
    float content_h = av_d
                    + kGap
                    + kNameH
                    + kGap * 0.5f
                    + kSecondaryH
                    + kGap;
    if (!is_dm)
    {
        content_h += kInvitedByH + kGap;
    }
    content_h += btn_row_h + kPadY;

    // Centre the content block horizontally and vertically.
    const float cx = bounds.x + (bounds.w - kContentW) * 0.5f;
    float       cy = bounds.y + std::max(0.0f, (bounds.h - content_h) * 0.5f);

    // Skip past avatar + text rows to reach buttons.
    cy += av_d + kGap + kNameH + kGap * 0.5f + kSecondaryH + kGap;
    if (!is_dm)
    {
        cy += kInvitedByH + kGap * 0.5f;
    }

    // Button row.
    float bx = cx;
    if (accept_btn_)
    {
        accept_btn_->arrange(lc, {bx, cy, btn_w, kBtnH});
        bx += btn_w + kBtnGap;
    }
    if (decline_btn_)
    {
        decline_btn_->arrange(lc, {bx, cy, btn_w, kBtnH});
        bx += btn_w + kBtnGap;
    }
    if (block_btn_ && is_dm)
    {
        block_btn_->arrange(lc, {bx, cy, btn_w, kBtnH});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void InviteCard::paint(tk::PaintCtx& ctx)
{
    if (!invite_.has_value())
    {
        return;
    }

    const auto& pal  = ctx.theme.palette;
    auto&       cv   = ctx.canvas;
    const bool is_dm = invite_->is_direct;
    const float av_d = is_dm ? kAvatarD_DM : kAvatarD_GR;

    // Background.
    cv.fill_rect(bounds_, pal.bg);

    // ── Content block ──────────────────────────────────────────────────────

    // Estimate content height (same as arrange) for vertical centering.
    float content_h = av_d + kGap + kNameH + kGap * 0.5f + kSecondaryH + kGap;
    if (!is_dm)
    {
        content_h += kInvitedByH + kGap * 0.5f;
    }
    content_h += kBtnH + kPadY;

    const float cx = bounds_.x + (bounds_.w - kContentW) * 0.5f;
    float       cy = bounds_.y +
                     std::max(0.0f, (bounds_.h - content_h) * 0.5f);

    // ── Avatar ─────────────────────────────────────────────────────────────

    const std::string& av_mxc = is_dm ? invite_->inviter_avatar_url
                                       : invite_->room_avatar_url;
    const std::string& fallback_name = is_dm ? invite_->inviter_display_name
                                              : invite_->room_name;

    const tk::Point av_centre{cx + kContentW * 0.5f, cy + av_d * 0.5f};
    const tk::Image* av_img = nullptr;
    if (image_provider_ && !av_mxc.empty())
    {
        av_img = image_provider_(av_mxc);
    }

    if (av_img)
    {
        cv.draw_circle_image(*av_img, av_centre, av_d);
    }
    else
    {
        std::string_view disp = fallback_name.empty()
                                    ? std::string_view("?")
                                    : std::string_view(fallback_name);
        cv.draw_initials_circle(disp, av_centre, av_d,
                                pal.accent,
                                tk::Color{255, 255, 255, 255});
    }

    cy += av_d + kGap;

    // ── Display name ───────────────────────────────────────────────────────

    const std::string& name_str = is_dm
        ? (invite_->inviter_display_name.empty() ? invite_->inviter_user_id
                                                 : invite_->inviter_display_name)
        : (invite_->room_name.empty() ? invite_->room_id
                                      : invite_->room_name);

    if (!name_layout_)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Title;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = kContentW;
        name_layout_ = ctx.factory.build_text(name_str, ts);
    }
    if (name_layout_)
    {
        const tk::Size sz = name_layout_->measure();
        const float tx    = cx + (kContentW - sz.w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, cy}, pal.text_primary);
        cy += sz.h + kGap * 0.5f;
    }
    else
    {
        cy += kNameH + kGap * 0.5f;
    }

    // ── Secondary line: @user_id (DM) or topic (group) ─────────────────────

    const std::string& secondary_str = is_dm
        ? invite_->inviter_user_id
        : invite_->room_topic;

    if (!secondary_str.empty())
    {
        if (!secondary_layout_)
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Body;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = kContentW;
            secondary_layout_ = ctx.factory.build_text(secondary_str, ts);
        }
        if (secondary_layout_)
        {
            const tk::Size sz = secondary_layout_->measure();
            const float tx    = cx + (kContentW - sz.w) * 0.5f;
            cv.draw_text(*secondary_layout_, {tx, cy}, pal.text_muted);
            cy += sz.h + kGap;
        }
        else
        {
            cy += kSecondaryH + kGap;
        }
    }
    else
    {
        cy += kSecondaryH + kGap;
    }

    // ── "Invited by …" line (group variant only) ───────────────────────────

    if (!is_dm)
    {
        const std::string inviter_name =
            invite_->inviter_display_name.empty()
                ? invite_->inviter_user_id
                : invite_->inviter_display_name;
        const std::string invited_by_str = "Invited by " + inviter_name;

        if (!invited_by_layout_)
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Small;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = kContentW;
            invited_by_layout_ = ctx.factory.build_text(invited_by_str, ts);
        }
        if (invited_by_layout_)
        {
            const tk::Size sz = invited_by_layout_->measure();
            const float tx    = cx + (kContentW - sz.w) * 0.5f;
            cv.draw_text(*invited_by_layout_, {tx, cy}, pal.text_muted);
            cy += sz.h + kGap;
        }
        else
        {
            cy += kInvitedByH + kGap;
        }
    }

    // ── Buttons ────────────────────────────────────────────────────────────

    if (accept_btn_)
    {
        accept_btn_->paint(ctx);
    }
    if (decline_btn_)
    {
        decline_btn_->paint(ctx);
    }
    if (block_btn_ && block_btn_->visible())
    {
        block_btn_->paint(ctx);
    }
}

} // namespace tesseract::views
