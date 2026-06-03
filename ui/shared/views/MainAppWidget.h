#pragma once

// Cross-platform main-application view. Owns all chat-UI widgets in a
// single tk::Widget tree so the host can mount it on one Surface and
// show/hide it as a unit — swapping in other screens is a Surface swap.
//
// Layout — horizontal split:
//   Sidebar (kSidebarW fixed)
//     Space-nav bar (kSpaceNavH, hidden until navigating a space)
//     RoomListView (flex)
//     UserInfo (kUserStripH)
//   1px Separator
//   Chat panel (flex)
//     VerificationBanner (kBannerH…124, hidden by default)
//     RoomView (flex)
//   ImageViewerOverlay (full widget bounds, hidden by default)
//   VideoViewerOverlay (full widget bounds, hidden by default)
//
// Shell wires two native overlays via the surface's set_on_layout():
//   compose_text_area_rect()    → NativeTextArea position
//   room_search_field_rect()    → NativeTextField position

#include "ConfirmDialog.h"
#include "EncryptionSetupOverlay.h"
#include "ImageViewerOverlay.h"
#include "InviteCard.h"
#include "QuickSwitcher.h"
#include "RoomListView.h"
#include "RoomView.h"
#include "UserInfo.h"
#include "VerificationBanner.h"
#include "VideoViewerOverlay.h"

#include "tk/controls.h"
#include "tk/tab_bar.h"
#include "tk/widget.h"

#include <tesseract/visual.h>

#include <functional>
#include <string>
#include <string_view>

namespace tesseract::views
{

class MainAppWidget : public tk::Widget
{
public:
    MainAppWidget();
    ~MainAppWidget() override = default;

    // ── Space navigation bar ──────────────────────────────────────────────

    // Show/hide the space-nav bar at the top of the sidebar.
    // Pass show=false to return to the plain room list.
    void set_space_nav(bool show, std::string_view space_name = {},
                       std::string_view avatar_url = {});

    // Supply the image cache lookup function used to paint the space avatar.
    // The signature matches the one used by RoomListView / RoomView.
    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& mxc_url)> provider);

    // ── Banner visibility ─────────────────────────────────────────────────

    void show_verif_banner(bool show);

    // ── Chat panel content switching ──────────────────────────────────────

    // Show the invite card for a pending invitation, hiding RoomView.
    void show_invite(const tesseract::InviteInfo& invite,
                     InviteCard::ImageProvider provider);

    // Show RoomView, hiding the invite card.
    void show_room();

    // Hide both RoomView and the invite card (nothing selected state).
    void clear_content();

    // ── Lightbox overlays ─────────────────────────────────────────────────

    void show_image_viewer(bool show);
    void show_video_viewer(bool show);
    void show_encryption_setup(bool show);

    // ── Quick switcher (Ctrl+K) ───────────────────────────────────────────

    void show_quick_switch(bool show);
    QuickSwitcher* quick_switcher() const { return quick_switcher_; }
    bool     quick_switch_field_visible() const;
    tk::Rect quick_switch_field_rect()    const;

    EncryptionSetupOverlay* encryption_setup() const { return encryption_setup_; }

    // Field-rect delegation — called from the shell's layout hook.
    bool     encryption_setup_passphrase_field_visible() const;
    tk::Rect encryption_setup_passphrase_field_rect()    const;
    bool     encryption_setup_key_field_visible()        const;
    tk::Rect encryption_setup_key_field_rect()           const;

    // ── Sub-view accessors ────────────────────────────────────────────────

    RoomListView* room_list_view() const
    {
        return room_list_view_;
    }
    RoomView* room_view() const
    {
        return room_view_;
    }
    InviteCard* invite_card() const
    {
        return invite_card_;
    }
    VerificationBanner* verif_banner() const
    {
        return verif_banner_;
    }
    ImageViewerOverlay* image_viewer() const
    {
        return img_viewer_;
    }
    VideoViewerOverlay* video_viewer() const
    {
        return vid_viewer_;
    }
    ConfirmDialog* confirm_dialog() const
    {
        return confirm_dialog_;
    }
    UserInfo* user_info() const
    {
        return user_info_;
    }
    tk::TabBar* tab_bar() const
    {
        return tab_bar_;
    }

    // Show/hide the tab bar and toggle the RoomHeader into condensed mode.
    // Call from on_tab_state_changed_ui_() whenever tabs_.size() changes
    // between 1 and 2 (or vice-versa).
    void set_tab_bar_visible(bool visible);

    // ── Native overlay rects (call from the surface's set_on_layout) ──────
    //
    // While any modal overlay is up (ConfirmDialog at this level, or the
    // room-info / user-profile panels owned by RoomView) the compose-textarea
    // and room-search accessors report "hidden" so the shells don't leave
    // those native OS controls clickable through the panel backdrop.

    tk::Rect compose_text_area_rect() const;
    bool room_search_field_visible() const;
    tk::Rect room_search_field_rect() const;

    // ── Callbacks ─────────────────────────────────────────────────────────

    // Fires when the user taps ← in the space nav bar.
    std::function<void()> on_space_back;

    // ── tk::Widget overrides ──────────────────────────────────────────────

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    // True when ConfirmDialog or any RoomView-owned panel covers the canvas;
    // drives compose_text_area_rect() / room_search_field_visible() so the
    // native OS controls hide while overlays are up.
    bool any_modal_open_() const;

    static constexpr float kSidebarW =
        static_cast<float>(tesseract::visual::kSidebarWidth);
    static constexpr float kSepW = 1.0f;
    static constexpr float kSpaceNavH = 36.0f;
    static constexpr float kUserStripH =
        static_cast<float>(tesseract::visual::kUserStripHeight);
    static constexpr float kBannerH = 48.0f;

    static constexpr float kNavAvatarSize = 24.0f;

    bool space_nav_visible_ = false;
    std::string space_name_;
    std::string avatar_url_;
    std::function<const tk::Image*(const std::string&)> avatar_provider_;

    // Sidebar children — borrowed raw pointers back from add_child()
    tk::Button* nav_back_btn_ = nullptr;
    tk::Label* nav_name_lbl_ = nullptr;
    RoomListView* room_list_view_ = nullptr;
    UserInfo* user_info_ = nullptr;

    // Chat panel children
    VerificationBanner* verif_banner_ = nullptr;
    tk::TabBar* tab_bar_ = nullptr;
    bool tab_bar_visible_ = false;
    RoomView* room_view_ = nullptr;
    InviteCard* invite_card_ = nullptr;

    // Full-surface lightbox overlays (painted last — highest z-order)
    ImageViewerOverlay* img_viewer_ = nullptr;
    VideoViewerOverlay* vid_viewer_ = nullptr;
    EncryptionSetupOverlay* encryption_setup_ = nullptr;

    // Modal confirmation overlay — sits above everything else, including the
    // lightboxes, so destructive prompts are always reachable.
    ConfirmDialog* confirm_dialog_ = nullptr;

    // Ctrl+K quick switcher — topmost overlay (added/painted after everything
    // else). Hidden until show_quick_switch(true).
    QuickSwitcher* quick_switcher_ = nullptr;

    bool verif_visible_ = false;
};

} // namespace tesseract::views
