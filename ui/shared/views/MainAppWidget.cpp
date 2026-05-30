#include "MainAppWidget.h"

#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <memory>

namespace tesseract::views
{

MainAppWidget::MainAppWidget()
{
    // Space nav bar: back button + space name label.
    // Both start hidden; set_space_nav() shows them.
    auto back = std::make_unique<tk::Button>(
        "←",
        [this]
        {
            if (on_space_back)
            {
                on_space_back();
            }
        },
        tk::Button::Variant::Subtle);
    nav_back_btn_ = add_child(std::move(back));
    nav_back_btn_->set_visible(false);

    auto name = std::make_unique<tk::Label>("", tk::FontRole::Body);
    nav_name_lbl_ = add_child(std::move(name));
    nav_name_lbl_->set_halign(tk::TextHAlign::Center);
    nav_name_lbl_->set_visible(false);

    // Sidebar: room list fills available space above the user strip.
    auto rlv = std::make_unique<RoomListView>();
    room_list_view_ = add_child(std::move(rlv));

    // Sidebar footer: logged-in user identity strip.
    auto ui = std::make_unique<UserInfo>();
    user_info_ = add_child(std::move(ui));

    // Chat panel: banners (hidden by default).
    auto ver = std::make_unique<VerificationBanner>();
    verif_banner_ = add_child(std::move(ver));
    verif_banner_->set_visible(false);

    // Chat panel: tab bar (hidden until 2+ tabs open).
    auto tb = std::make_unique<tk::TabBar>();
    tab_bar_ = add_child(std::move(tb));
    tab_bar_->set_visible(false);

    // Chat panel: main room view (header + messages + compose bar).
    auto rv = std::make_unique<RoomView>();
    room_view_ = add_child(std::move(rv));

    // Chat panel: invite card (shown instead of room_view_ for pending invites).
    auto ic = std::make_unique<InviteCard>();
    invite_card_ = add_child(std::move(ic));
    // InviteCard starts invisible (clear() is called in its constructor).

    // Full-surface lightbox overlays — added last so they win hit-testing
    // and are painted on top of everything else when visible.
    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = add_child(std::move(vid));
    vid_viewer_->set_visible(false);

    auto enc = std::make_unique<EncryptionSetupOverlay>(EncryptionSetupOverlay::Mode::Fresh);
    encryption_setup_ = add_child(std::move(enc));
    encryption_setup_->set_visible(false);

    // Modal confirmation overlay — added after the lightboxes so it paints
    // on top of *everything*. Visibility is gated by ConfirmDialog::open()
    // / close() so an idle dialog doesn't capture hit-tests.
    auto confirm = std::make_unique<ConfirmDialog>();
    confirm_dialog_ = add_child(std::move(confirm));

    // Hand RoomView a closure that opens this dialog with caller-supplied
    // options. Downstream destructive actions (leave room, …) route through
    // this provider without each shell needing its own native dialog code.
    if (room_view_ && confirm_dialog_)
    {
        room_view_->set_confirm_provider(
            [this](ConfirmDialog::Options opts,
                   std::function<void()> on_confirm) {
                confirm_dialog_->open(std::move(opts), std::move(on_confirm));
            });

        // Route the dialog's layout-changed notification through RoomView's
        // shared on_layout_changed chain so the shell hides the compose
        // textarea + room-search overlays while the dialog is up.
        confirm_dialog_->on_layout_changed = [this]() {
            if (room_view_ && room_view_->on_layout_changed)
                room_view_->on_layout_changed();
        };
    }
}

// ── Visibility controls ────────────────────────────────────────────────────

void MainAppWidget::set_space_nav(bool show, std::string_view space_name,
                                  std::string_view avatar_url)
{
    space_nav_visible_ = show;
    space_name_ = show ? std::string(space_name) : std::string{};
    avatar_url_ = show ? std::string(avatar_url) : std::string{};
    if (nav_back_btn_)
    {
        nav_back_btn_->set_visible(show);
    }
    if (nav_name_lbl_)
    {
        nav_name_lbl_->set_visible(show);
        nav_name_lbl_->set_text(std::string(space_name));
    }
}

void MainAppWidget::set_avatar_provider(
    std::function<const tk::Image*(const std::string& mxc_url)> provider)
{
    avatar_provider_ = std::move(provider);
}

void MainAppWidget::show_verif_banner(bool show)
{
    verif_visible_ = show;
    if (verif_banner_)
    {
        verif_banner_->set_visible(show);
    }
}

void MainAppWidget::set_tab_bar_visible(bool visible)
{
    tab_bar_visible_ = visible;
    if (tab_bar_)
    {
        tab_bar_->set_visible(visible);
    }
    if (room_view_ && room_view_->header())
    {
        room_view_->header()->set_condensed(visible);
    }
}

void MainAppWidget::show_invite(const tesseract::InviteInfo& invite,
                                InviteCard::ImageProvider provider)
{
    if (invite_card_)
        invite_card_->set_invite(invite, std::move(provider));
    if (room_view_)
        room_view_->set_visible(false);
}

void MainAppWidget::show_room()
{
    if (invite_card_)
        invite_card_->clear();
    if (room_view_)
        room_view_->set_visible(true);
}

void MainAppWidget::clear_content()
{
    if (invite_card_)
        invite_card_->clear();
    if (room_view_)
    {
        room_view_->clear_room();
        room_view_->set_visible(true);
    }
}

void MainAppWidget::show_image_viewer(bool show)
{
    if (img_viewer_)
    {
        img_viewer_->set_visible(show);
    }
}

void MainAppWidget::show_video_viewer(bool show)
{
    if (vid_viewer_)
    {
        vid_viewer_->set_visible(show);
    }
}

void MainAppWidget::show_encryption_setup(bool show)
{
    if (encryption_setup_)
    {
        encryption_setup_->set_visible(show);
    }
}

bool MainAppWidget::encryption_setup_passphrase_field_visible() const
{
    return encryption_setup_ && encryption_setup_->passphrase_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_passphrase_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->passphrase_field_rect_value();
}

bool MainAppWidget::encryption_setup_key_field_visible() const
{
    return encryption_setup_ && encryption_setup_->key_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_key_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->key_field_rect_value();
}

// ── Native overlay rect queries ────────────────────────────────────────────

bool MainAppWidget::any_modal_open_() const
{
    return (confirm_dialog_    && confirm_dialog_->is_open()) ||
           (room_view_         && room_view_->is_overlay_open()) ||
           (img_viewer_        && img_viewer_->is_open()) ||
           (vid_viewer_        && vid_viewer_->is_open()) ||
           (encryption_setup_  && encryption_setup_->visible());
}

tk::Rect MainAppWidget::compose_text_area_rect() const
{
    // While a modal covers the canvas the native textarea must not steal
    // focus or clicks — report an empty rect so the shell hides the overlay.
    if (any_modal_open_()) return {};
    return room_view_ ? room_view_->compose_text_area_rect() : tk::Rect{};
}

bool MainAppWidget::room_search_field_visible() const
{
    if (any_modal_open_()) return false;
    return room_list_view_ && room_list_view_->search_field_visible();
}

tk::Rect MainAppWidget::room_search_field_rect() const
{
    return room_list_view_ ? room_list_view_->search_field_rect() : tk::Rect{};
}

// ── tk::Widget overrides ───────────────────────────────────────────────────

tk::Size MainAppWidget::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void MainAppWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const float x = bounds.x;
    const float y = bounds.y;
    const float w = bounds.w;
    const float h = bounds.h;

    // ── Sidebar ──────────────────────────────────────────────────────────

    float sidebar_content_y = y;

    if (space_nav_visible_)
    {
        constexpr float kBtnW = 32.0f;
        constexpr float kPad = 4.0f;
        const float btn_y = y + (kSpaceNavH - 24.0f) / 2.0f;
        nav_back_btn_->arrange(ctx, {x + kPad, btn_y, kBtnW, 24.0f});
        // Label spans the full sidebar width with center halign, but
        // Label::paint draws at bounds_.y so we must vertically centre the
        // rect ourselves.  kNameH = 18 px matches the body-font line height
        // used by RoomHeader; centering it in kSpaceNavH gives 9 px margins.
        constexpr float kNameH = 18.0f;
        nav_name_lbl_->arrange(
            ctx,
            {x, y + (kSpaceNavH - kNameH) * 0.5f, kSidebarW, kNameH});
        sidebar_content_y = y + kSpaceNavH;
    }

    // User strip is pinned to the bottom of the sidebar.
    const float user_strip_y = y + h - kUserStripH;
    user_info_->arrange(ctx, {x, user_strip_y, kSidebarW, kUserStripH});

    // Room list fills the space between nav bar (or top) and user strip.
    const float room_list_h = std::max(0.0f, user_strip_y - sidebar_content_y);
    room_list_view_->arrange(ctx,
                             {x, sidebar_content_y, kSidebarW, room_list_h});

    // ── Chat panel ───────────────────────────────────────────────────────

    const float chat_x = x + kSidebarW + kSepW;
    const float chat_w = w - kSidebarW - kSepW;
    float chat_y = y;

    if (verif_visible_)
    {
        // VerificationBanner is taller when showing the emoji grid.
        const float verif_h =
            verif_banner_->measure(ctx, {chat_w, h - (chat_y - y)}).h;
        verif_banner_->arrange(ctx, {chat_x, chat_y, chat_w, verif_h});
        chat_y += verif_h;
    }

    if (tab_bar_visible_ && tab_bar_)
    {
        tab_bar_->arrange(ctx, {chat_x, chat_y, chat_w, tk::TabBar::kHeight});
        chat_y += tk::TabBar::kHeight;
    }

    const float room_h = std::max(0.0f, y + h - chat_y);
    const tk::Rect chat_content_rect{chat_x, chat_y, chat_w, room_h};
    room_view_->arrange(ctx, chat_content_rect);
    if (invite_card_)
    {
        invite_card_->arrange(ctx, chat_content_rect);
    }

    // ── Full-surface overlays (always at full widget bounds) ─────────────

    img_viewer_->arrange(ctx, bounds);
    vid_viewer_->arrange(ctx, bounds);
    if (encryption_setup_) encryption_setup_->arrange(ctx, bounds);
    if (confirm_dialog_) confirm_dialog_->arrange(ctx, bounds);
}

void MainAppWidget::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Sidebar background.
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, kSidebarW, bounds_.h},
                         pal.sidebar_bg);

    // User strip footer: same background as sidebar + 1px top border.
    const float strip_y = bounds_.y + bounds_.h - kUserStripH;
    ctx.canvas.fill_rect({bounds_.x, strip_y, kSidebarW, kUserStripH},
                         pal.sidebar_bg);
    ctx.canvas.fill_rect({bounds_.x, strip_y, kSidebarW, 1.0f}, pal.separator);

    // Space nav bar background (overlays the top of the sidebar).
    if (space_nav_visible_)
    {
        ctx.canvas.fill_rect({bounds_.x, bounds_.y, kSidebarW, kSpaceNavH},
                             pal.chrome_bg);
        if (nav_name_lbl_)
        {
            nav_name_lbl_->paint(ctx);
        }
        // Space avatar (circle image or initials disc) just right of the
        // back button, painted on top of the label.
        constexpr float kBtnW = 32.0f;
        constexpr float kPad = 4.0f;
        const tk::Point avatar_centre{
            bounds_.x + kPad + kBtnW + kPad + kNavAvatarSize * 0.5f,
            bounds_.y + kSpaceNavH * 0.5f};
        if (avatar_provider_ && !avatar_url_.empty())
        {
            if (const tk::Image* img = avatar_provider_(avatar_url_))
            {
                ctx.canvas.draw_circle_image(*img, avatar_centre, kNavAvatarSize);
            }
            else
            {
                ctx.canvas.draw_initials_circle(
                    space_name_, avatar_centre, kNavAvatarSize,
                    pal.avatar_initials_bg, pal.avatar_initials_text);
            }
        }
        else if (!space_name_.empty())
        {
            ctx.canvas.draw_initials_circle(
                space_name_, avatar_centre, kNavAvatarSize,
                pal.avatar_initials_bg, pal.avatar_initials_text);
        }
        // Back button painted last so it renders above the label and avatar.
        if (nav_back_btn_)
        {
            nav_back_btn_->paint(ctx);
        }
    }

    // Sidebar content.
    if (room_list_view_)
    {
        room_list_view_->paint(ctx);
    }
    if (user_info_)
    {
        user_info_->paint(ctx);
    }

    // 1px vertical separator between sidebar and chat panel.
    ctx.canvas.fill_rect({bounds_.x + kSidebarW, bounds_.y, kSepW, bounds_.h},
                         pal.separator);

    // Chat panel.
    if (verif_visible_ && verif_banner_)
    {
        verif_banner_->paint(ctx);
    }
    if (tab_bar_visible_ && tab_bar_)
    {
        tab_bar_->paint(ctx);
    }
    if (room_view_ && room_view_->visible())
    {
        room_view_->paint(ctx);
    }
    if (invite_card_ && invite_card_->visible())
    {
        invite_card_->paint(ctx);
    }

    // Lightbox overlays (painted last — on top of everything).
    if (img_viewer_ && img_viewer_->visible())
    {
        img_viewer_->paint(ctx);
    }
    if (vid_viewer_ && vid_viewer_->visible())
    {
        vid_viewer_->paint(ctx);
    }
    if (encryption_setup_ && encryption_setup_->visible())
    {
        encryption_setup_->paint(ctx);
    }
    // Modal confirmation — drawn above the lightboxes so destructive prompts
    // are still reachable when the image/video viewer is up.
    if (confirm_dialog_ && confirm_dialog_->visible())
    {
        confirm_dialog_->paint(ctx);
    }
}

} // namespace tesseract::views
