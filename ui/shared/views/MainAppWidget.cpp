#include "MainAppWidget.h"

#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <memory>

namespace tesseract::views {

MainAppWidget::MainAppWidget() {
    // Space nav bar: back button + space name label.
    // Both start hidden; set_space_nav() shows them.
    auto back = std::make_unique<tk::Button>("←",
        [this] { if (on_space_back) on_space_back(); },
        tk::Button::Variant::Subtle);
    nav_back_btn_ = add_child(std::move(back));
    nav_back_btn_->set_visible(false);

    auto name = std::make_unique<tk::Label>("", tk::FontRole::Body);
    nav_name_lbl_ = add_child(std::move(name));
    nav_name_lbl_->set_visible(false);

    // Sidebar: room list fills available space above the user strip.
    auto rlv = std::make_unique<RoomListView>();
    room_list_view_ = add_child(std::move(rlv));

    // Sidebar footer: logged-in user identity strip.
    auto ui = std::make_unique<UserInfo>();
    user_info_ = add_child(std::move(ui));

    // Chat panel: banners (hidden by default).
    auto rec = std::make_unique<RecoveryBanner>();
    recovery_banner_ = add_child(std::move(rec));
    recovery_banner_->set_visible(false);

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

    // Full-surface lightbox overlays — added last so they win hit-testing
    // and are painted on top of everything else when visible.
    auto img = std::make_unique<ImageViewerOverlay>();
    img_viewer_ = add_child(std::move(img));
    img_viewer_->set_visible(false);

    auto vid = std::make_unique<VideoViewerOverlay>();
    vid_viewer_ = add_child(std::move(vid));
    vid_viewer_->set_visible(false);
}

// ── Visibility controls ────────────────────────────────────────────────────

void MainAppWidget::set_space_nav(bool show, std::string_view space_name) {
    space_nav_visible_ = show;
    if (nav_back_btn_) nav_back_btn_->set_visible(show);
    if (nav_name_lbl_) {
        nav_name_lbl_->set_visible(show);
        nav_name_lbl_->set_text(std::string(space_name));
    }
}

void MainAppWidget::show_recovery_banner(bool show) {
    recovery_visible_ = show;
    if (recovery_banner_) recovery_banner_->set_visible(show);
}

void MainAppWidget::show_verif_banner(bool show) {
    verif_visible_ = show;
    if (verif_banner_) verif_banner_->set_visible(show);
}

void MainAppWidget::set_tab_bar_visible(bool visible) {
    tab_bar_visible_ = visible;
    if (tab_bar_) tab_bar_->set_visible(visible);
    if (room_view_ && room_view_->header())
        room_view_->header()->set_condensed(visible);
}

void MainAppWidget::show_image_viewer(bool show) {
    if (img_viewer_) img_viewer_->set_visible(show);
}

void MainAppWidget::show_video_viewer(bool show) {
    if (vid_viewer_) vid_viewer_->set_visible(show);
}

// ── Native overlay rect queries ────────────────────────────────────────────

tk::Rect MainAppWidget::compose_text_area_rect() const {
    return room_view_ ? room_view_->compose_text_area_rect() : tk::Rect{};
}

bool MainAppWidget::room_search_field_visible() const {
    return room_list_view_ && room_list_view_->search_field_visible();
}

tk::Rect MainAppWidget::room_search_field_rect() const {
    return room_list_view_ ? room_list_view_->search_field_rect() : tk::Rect{};
}

bool MainAppWidget::recovery_key_field_visible() const {
    return recovery_visible_ && recovery_banner_
        && recovery_banner_->recovery_key_field_visible();
}

tk::Rect MainAppWidget::recovery_key_field_rect() const {
    if (!recovery_visible_ || !recovery_banner_) return {};
    return recovery_banner_->recovery_key_field_rect();
}

// ── tk::Widget overrides ───────────────────────────────────────────────────

tk::Size MainAppWidget::measure(tk::LayoutCtx&, tk::Size constraints) {
    return constraints;
}

void MainAppWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    const float x = bounds.x;
    const float y = bounds.y;
    const float w = bounds.w;
    const float h = bounds.h;

    // ── Sidebar ──────────────────────────────────────────────────────────

    float sidebar_content_y = y;

    if (space_nav_visible_) {
        constexpr float kBtnW = 32.0f;
        constexpr float kPad  = 4.0f;
        const float btn_y = y + (kSpaceNavH - 24.0f) / 2.0f;
        nav_back_btn_->arrange(ctx,
            { x + kPad, btn_y, kBtnW, 24.0f });
        nav_name_lbl_->arrange(ctx,
            { x + kPad + kBtnW + kPad, y,
              kSidebarW - kBtnW - kPad * 3.0f, kSpaceNavH });
        sidebar_content_y = y + kSpaceNavH;
    }

    // User strip is pinned to the bottom of the sidebar.
    const float user_strip_y = y + h - kUserStripH;
    user_info_->arrange(ctx, { x, user_strip_y, kSidebarW, kUserStripH });

    // Room list fills the space between nav bar (or top) and user strip.
    const float room_list_h = std::max(0.0f, user_strip_y - sidebar_content_y);
    room_list_view_->arrange(ctx,
        { x, sidebar_content_y, kSidebarW, room_list_h });

    // ── Chat panel ───────────────────────────────────────────────────────

    const float chat_x = x + kSidebarW + kSepW;
    const float chat_w = w - kSidebarW - kSepW;
    float chat_y = y;

    if (recovery_visible_) {
        recovery_banner_->arrange(ctx,
            { chat_x, chat_y, chat_w, kBannerH });
        chat_y += kBannerH;
    }

    if (verif_visible_) {
        // VerificationBanner is taller when showing the emoji grid.
        const float verif_h =
            verif_banner_->measure(ctx, { chat_w, h - (chat_y - y) }).h;
        verif_banner_->arrange(ctx,
            { chat_x, chat_y, chat_w, verif_h });
        chat_y += verif_h;
    }

    if (tab_bar_visible_ && tab_bar_) {
        tab_bar_->arrange(ctx,
            { chat_x, chat_y, chat_w, tk::TabBar::kHeight });
        chat_y += tk::TabBar::kHeight;
    }

    const float room_h = std::max(0.0f, y + h - chat_y);
    room_view_->arrange(ctx, { chat_x, chat_y, chat_w, room_h });

    // ── Full-surface overlays (always at full widget bounds) ─────────────

    img_viewer_->arrange(ctx, bounds);
    vid_viewer_->arrange(ctx, bounds);
}

void MainAppWidget::paint(tk::PaintCtx& ctx) {
    const auto& pal = ctx.theme.palette;

    // Sidebar background.
    ctx.canvas.fill_rect(
        { bounds_.x, bounds_.y, kSidebarW, bounds_.h },
        pal.sidebar_bg);

    // User strip footer: same background as sidebar + 1px top border.
    const float strip_y = bounds_.y + bounds_.h - kUserStripH;
    ctx.canvas.fill_rect(
        { bounds_.x, strip_y, kSidebarW, kUserStripH },
        pal.sidebar_bg);
    ctx.canvas.fill_rect(
        { bounds_.x, strip_y, kSidebarW, 1.0f },
        pal.separator);

    // Space nav bar background (overlays the top of the sidebar).
    if (space_nav_visible_) {
        ctx.canvas.fill_rect(
            { bounds_.x, bounds_.y, kSidebarW, kSpaceNavH },
            pal.chrome_bg);
        if (nav_back_btn_) nav_back_btn_->paint(ctx);
        if (nav_name_lbl_) nav_name_lbl_->paint(ctx);
    }

    // Sidebar content.
    if (room_list_view_) room_list_view_->paint(ctx);
    if (user_info_)      user_info_->paint(ctx);

    // 1px vertical separator between sidebar and chat panel.
    ctx.canvas.fill_rect(
        { bounds_.x + kSidebarW, bounds_.y, kSepW, bounds_.h },
        pal.separator);

    // Chat panel.
    if (recovery_visible_ && recovery_banner_) recovery_banner_->paint(ctx);
    if (verif_visible_    && verif_banner_)    verif_banner_->paint(ctx);
    if (tab_bar_visible_  && tab_bar_)         tab_bar_->paint(ctx);
    if (room_view_)                            room_view_->paint(ctx);

    // Lightbox overlays (painted last — on top of everything).
    if (img_viewer_ && img_viewer_->visible()) img_viewer_->paint(ctx);
    if (vid_viewer_ && vid_viewer_->visible()) vid_viewer_->paint(ctx);
}

} // namespace tesseract::views
