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
//     VerificationBanner (variable height, hidden by default)
//     RoomView (flex)
//   ImageViewerOverlay (full widget bounds, hidden by default)
//   VideoViewerOverlay (full widget bounds, hidden by default)
//
// Below kNarrowBreakpoint total width, RootLayoutWidget collapses to a
// single full-width pane (list or room) instead of the split above — see
// RootLayoutWidget::arrange() and show_room_list_pane_narrow_().
//
// The compose bar's text input and the room-search field are both
// self-positioning (tk::TextArea / tk::TextField reached via
// room_view()->compose_bar()->text_area() and
// room_list_view()->search_field()); arrange() force-hides them while
// any_modal_open_() or while their owning pane is hidden (narrow mode) — see
// compose_text_area_rect().

#include "ConfirmDialog.h"
#include "CameraWidget.h"
#include "EncryptionSetupOverlay.h"
#include "ImageViewerOverlay.h"
#include "QRGrantView.h"
#include "InviteCard.h"
#include "RoomPreviewView.h"
#include "SpaceRootView.h"
#include "ForwardRoomPicker.h"
#include "AddRoomView.h"
#include "QuickSwitcher.h"
#include "MessageSearchView.h"
#include "RoomMediaView.h"
#include "RoomListView.h"
#include "RoomView.h"
#include "UserInfo.h"
#include "VerificationBanner.h"
#include "VideoViewerOverlay.h"
#include "CallOverlayWidget.h"
#include "ScreenPickerWidget.h"

#include "tk/controls.h"
#include "tk/svg.h"
#include "tk/tab_bar.h"
#include "tk/widget.h"

#include <tesseract/visual.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>
#include <string>
#include <string_view>

namespace tesseract::views
{

class MainAppWidget : public tk::Widget
{
protected:
    // host() is borrowed for the widget's lifetime — used to hand native-overlay
    // fields (search/passphrase/check-code TextFields, ...) a Host at
    // construction time instead of waiting for the first paint(). Nullable:
    // when null, every such field is simply not constructed (stays nullptr),
    // matching tk::TextField's own headless-Host contract one level up — so
    // tests that don't care about a live native field can default-construct.
    MainAppWidget();
    TK_WIDGET_FACTORY_FRIEND(MainAppWidget)

public:
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

    // Show RoomView, hiding the invite card and room preview.
    void show_room();

    // Show the room-preview panel for an unjoined space child, hiding RoomView.
    void show_room_preview(const tesseract::RoomSummary& s,
                           RoomPreviewView::AvatarProvider provider);
    // Hide the room preview and restore RoomView.
    void hide_room_preview();

    // Show the joined-space root panel, hiding RoomView.
    void show_space_root(const tesseract::RoomInfo& space,
                         std::size_t joined_children,
                         std::size_t unjoined_children,
                         SpaceRootView::AvatarProvider provider);
    void hide_space_root();

    // Hide both RoomView and the invite card (nothing selected state).
    void clear_content();

    // ── Lightbox overlays ─────────────────────────────────────────────────

    void show_image_viewer(bool show);
    void show_video_viewer(bool show);

    // Open the selfie camera overlay. The shell should set on_selfie_captured
    // before calling this. Calling while one is already open is a no-op.
    void open_camera_overlay();

    // Called automatically when the overlay dismisses itself (post-capture or
    // user cancel). May also be called by the shell (e.g. on window close).
    void close_camera_overlay();

    bool camera_overlay_open() const { return camera_widget_ != nullptr; }

    // Set by the shell before calling open_camera_overlay(). Receives the raw
    // BGRA8888 frame; the shell encodes it to JPEG and calls set_pending_image.
    std::function<void(std::vector<std::uint8_t> bgra,
                       std::uint32_t w, std::uint32_t h)>
        on_selfie_captured;

    // Set by the shell. open_camera_overlay() is a no-op while this returns true.
    std::function<bool()> is_call_active;
    void show_encryption_setup(bool show);
    void show_qr_grant(bool show);
    QRGrantView* qr_grant_view() const { return qr_grant_view_; }

    // ── Quick switcher (Ctrl+K) ───────────────────────────────────────────

    std::function<void()> on_quick_switch_shortcut;
    void show_quick_switch(bool show);
    QuickSwitcher* quick_switcher() const { return quick_switcher_; }

    // ── Message search (Ctrl+Shift+F) ─────────────────────────────────────

    std::function<void()> on_message_search_shortcut;
    void show_message_search(bool show);
    MessageSearchView* message_search() const { return message_search_; }

    // ── Room media gallery ────────────────────────────────────────────────

    // RoomView-owned (not a MainAppWidget-level overlay) so it participates
    // in RoomView::active_overlay_panel_()'s Tab-scoping/pointer routing and
    // set_room()'s room-switch panel-closing for free — see RoomView.h's own
    // room_media_view_ doc comment. This accessor just delegates so every
    // existing caller (ShellBase, the 4 shells) keeps working unchanged.
    RoomMediaView* room_media_view() const
    {
        return room_view_ ? room_view_->room_media_view() : nullptr;
    }

    // ── Forward room picker ───────────────────────────────────────────────

    ForwardRoomPicker* forward_picker() const { return forward_picker_; }

    // Combined Join/Create "Add Room" dialog — opened by RoomListView's "+"
    // button (Join tab) or via ShellBase::open_matrix_link() for an
    // unjoined alias (Join tab, prefilled).
    AddRoomView* add_room_view() const { return add_room_view_; }

    EncryptionSetupOverlay* encryption_setup() const { return encryption_setup_; }

    // Configure and show the call overlay for an active session. Routes to
    // RoomView::mount_call_panel() for Docked/DockedExpanded modes, or creates
    // a floating CallOverlayWidget child for Floating mode.
    // post_delayed wires the timer, repaint_requester requests video-frame
    // repaints, avatar_provider and display_name_provider supply participant
    // metadata. Does not call start_timer() — the caller owns that.
    void mount_call_overlay(
        views::CallOverlayWidget::Mode                  initial_mode,
        std::function<void(int, std::function<void()>)> post_delayed,
        std::function<void()>                           repaint_requester,
        std::function<const tk::Image*(const std::string&)> avatar_provider,
        std::function<std::string(const std::string&)>  display_name_provider);

    // Tear down the overlay: stops the timer and removes/hides it.
    // Cleans up both the docked panel (via room_view_->unmount_call_panel())
    // and the floating overlay (via remove_child) if either is active.
    void unmount_call_overlay();

    // Returns the active CallOverlayWidget in the main room view (Docked /
    // DockedExpanded) or the floating layer (Floating mode), or nullptr.
    views::CallOverlayWidget* call_panel_for_room() const;

    // Show the screen source picker as a full-area modal overlay.
    // on_selected fires with the chosen source_id; on_cancelled fires if the
    // user dismisses without choosing. Both callbacks remove the picker.
    // Returns the mounted widget so the caller can push thumbnails into it as
    // they're captured; the pointer is invalidated once either callback fires.
    views::ScreenPickerWidget* mount_screen_picker(
        std::vector<tk::ScreenSource> sources,
        std::function<void(std::string)> on_selected,
        std::function<void()>            on_cancelled);

    // Remove the screen picker if currently shown.
    void unmount_screen_picker();

    // True while the screen picker modal is visible.
    bool screen_picker_open() const { return screen_picker_ != nullptr; }

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
    RoomPreviewView* room_preview() const
    {
        return room_preview_;
    }
    SpaceRootView* space_root() const
    {
        return space_root_;
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

    // ── Offline connectivity banner ───────────────────────────────────────────

    // Show or hide the "No internet connection" strip at the top of the chat
    // panel. Called by ShellBase when sync loses/regains network connectivity.
    void set_offline(bool offline);

    // ── Native overlay rects (call from the surface's set_on_layout) ──────
    //
    // While any modal overlay is up (ConfirmDialog at this level, or the
    // room-info / user-profile panels owned by RoomView) the compose-textarea
    // accessor reports "hidden" so the shells don't leave that native OS
    // control clickable through the panel backdrop.

    tk::Rect compose_text_area_rect() const;

    // ── Callbacks ─────────────────────────────────────────────────────────

    // Fires when the user taps ← in the space nav bar.
    std::function<void()> on_space_back;
    // Fires when the user presses Ctrl/Cmd+F in the canvas widget tree.
    std::function<void()> on_find_in_room_shortcut;
    // Fires for shared room-history navigation shortcuts.
    std::function<void()> on_history_back_shortcut;
    std::function<void()> on_history_forward_shortcut;
    // Fires when the user clicks the current space's avatar/name in the nav bar.
    std::function<void()> on_space_header;

    // ── tk::Widget overrides ──────────────────────────────────────────────

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool on_key_down(const tk::KeyEvent& event) override;

private:
    class SpaceNavWidget;
    class SidebarWidget;
    class OfflineBannerWidget;
    class ChatContentStack;
    class ChatPanelWidget;
    class RootLayoutWidget;
    class OverlayStackWidget;
    class FloatingCallLayerWidget;

    // True when ConfirmDialog or any RoomView-owned panel covers the canvas;
    // drives compose_text_area_rect() and the room-list search field's
    // visibility gating so the native OS controls hide while overlays are up.
    bool any_modal_open_() const;
    void clear_alternate_content_();
    void notify_layout_changed_();
    void set_room_visible_(bool visible);
    bool handle_primary_shortcut_(const tk::KeyEvent& event);
    bool handle_history_shortcut_(const tk::KeyEvent& event);
    bool dismiss_top_transient_();

    // Returns to the room-list pane when narrow and the room pane is
    // showing (closing any open in-room search first). No-op — returns
    // false — otherwise, including while any transient overlay is open, so
    // callers (RoomHeader's back button, Escape) never switch panes behind
    // a modal's back.
    bool show_room_list_pane_narrow_();

    // The topmost currently-open MainAppWidget-level transient overlay, or
    // nullptr — priority mirrors dismiss_top_transient_()'s visual stacking
    // order. Used by paint() to scope Tab/Shift-Tab traversal to it (see
    // paint()'s own comment for why RoomView's nested-panel scoping isn't
    // touched here).
    tk::Widget* active_transient_overlay_() const;

    static constexpr float kSidebarW =
        static_cast<float>(tesseract::visual::kSidebarWidth);
    static constexpr float kSepW = 1.0f;
    static constexpr float kSpaceNavH = 36.0f;
    static constexpr float kUserStripH =
        static_cast<float>(tesseract::visual::kUserStripHeight);
    static constexpr float kNarrowBreakpoint =
        static_cast<float>(tesseract::visual::kNarrowBreakpointWidth);

    static constexpr float kNavAvatarSize = 24.0f;

    // Primary split layout
    RootLayoutWidget* root_layout_ = nullptr;
    SidebarWidget* sidebar_ = nullptr;
    RoomListView* room_list_view_ = nullptr;
    UserInfo* user_info_ = nullptr;

    // Chat panel children
    ChatPanelWidget* chat_panel_ = nullptr;
    VerificationBanner* verif_banner_ = nullptr;
    tk::TabBar* tab_bar_ = nullptr;
    RoomView*        room_view_    = nullptr;
    InviteCard*      invite_card_  = nullptr;
    RoomPreviewView* room_preview_ = nullptr;
    SpaceRootView*   space_root_   = nullptr;

    bool modal_was_open_ = false;

    // Full-surface lightbox overlays (painted last — highest z-order)
    OverlayStackWidget* overlay_stack_ = nullptr;
    ImageViewerOverlay* img_viewer_ = nullptr;
    VideoViewerOverlay* vid_viewer_ = nullptr;
    // Selfie overlay — lazily created by open_camera_overlay(), freed on dismiss.
    CameraWidget* camera_widget_ = nullptr;
    EncryptionSetupOverlay* encryption_setup_ = nullptr;
    QRGrantView* qr_grant_view_ = nullptr;

    // Modal confirmation overlay — sits above everything else, including the
    // lightboxes, so destructive prompts are always reachable.
    ConfirmDialog* confirm_dialog_ = nullptr;

    // Ctrl+K quick switcher — topmost overlay (added/painted after everything
    // else). Hidden until show_quick_switch(true).
    QuickSwitcher* quick_switcher_ = nullptr;

    // Ctrl+Shift+F message search — topmost overlay alongside the quick
    // switcher. Hidden until show_message_search(true).
    MessageSearchView* message_search_ = nullptr;

    // Forward room picker — topmost modal overlay, shown when the user
    // selects "Forward message" from the action bar.
    ForwardRoomPicker* forward_picker_ = nullptr;
    AddRoomView* add_room_view_ = nullptr;


    FloatingCallLayerWidget* floating_call_layer_ = nullptr;
    // Floating-mode CallOverlayWidget — lazily created by mount_call_overlay()
    // when initial_mode == Floating, removed by unmount_call_overlay().
    // nullptr when no floating call is active. Ownership is in floating_call_layer_.
    views::CallOverlayWidget* float_call_overlay_ = nullptr;

    // Screen source picker — shown modally over the full app area while the
    // user selects what to share. Removed immediately on selection or cancel.
    views::ScreenPickerWidget* screen_picker_ = nullptr;


    static constexpr float kOfflineBannerH = 32.0f;
};

} // namespace tesseract::views
