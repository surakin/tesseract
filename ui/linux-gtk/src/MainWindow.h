#pragma once
#include <gtk/gtk.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>
#include "LinuxNotifier.h"
#include "LinuxUpConnectorGtk.h"
#include "LinuxGtkTrayIcon.h"

#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "tk/anim_image_cache.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/AccountPicker.h"
#include "views/EmojiPicker.h"
#include "views/format.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/RecoveryBanner.h"
#include "views/VerificationBanner.h"
#include "views/RoomListView.h"
#include "views/RoomView.h"
#include "views/StickerPicker.h"
#include "views/JoinRoomView.h"
#include "views/UserInfo.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gtk4 {

class LoginView;

// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();

    GtkWidget* widget() const { return window_; }

    // These are called from internal async callbacks (paginate/subscribe workers).
    void push_paginate_result(std::string room_id, bool reached_start);
    void push_subscribe_result(std::string room_id, bool reached_start);

private:
    // ── EventHandlerBase UI-thread hook overrides (GTK4) ──────────────────────
    void handle_timeline_reset_ui_(
        std::string room_id,
        std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void handle_message_inserted_ui_(
        std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_updated_ui_(
        std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_removed_ui_(
        std::string room_id, std::size_t index) override;
    void handle_sync_error_ui_(
        std::string context, std::string user_id,
        std::string description, bool soft_logout) override;
    void handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void handle_image_packs_updated_ui_() override;
    void handle_verification_request_ui_(
        std::string flow_id, std::string user_id,
        std::string device_id, bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(
        std::string flow_id, std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_account_prefs_updated_ui_(
        std::string user_id, std::string json) override;
    void handle_notification_ui_(
        std::string user_id, std::string room_id,
        std::string room_name, std::string sender,
        std::string body, bool is_mention,
        std::vector<uint8_t> avatar_bytes) override;
    void on_room_list_state_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;

    // ── Internal push helpers (called from handle_*_ui_ and async workers) ────
    void push_timeline_reset(std::string room_id,
                              std::vector<std::unique_ptr<tesseract::Event>> snapshot);
    void push_message_inserted(std::string room_id,
                                std::size_t index,
                                std::unique_ptr<tesseract::Event> ev);
    void push_message_updated(std::string room_id,
                               std::size_t index,
                               std::unique_ptr<tesseract::Event> ev);
    void push_message_removed(std::string room_id, std::size_t index);
    // user_id identifies which account's snapshot this is (for caching).
    void push_rooms(std::string user_id, std::vector<tesseract::RoomInfo> rooms);
    void push_error(std::string description);
    void handle_reconnect(const std::string& user_id);
    void handle_auth_error(bool soft_logout);
    void push_backup_progress(tesseract::BackupProgress progress);
    void push_room_list_state(tesseract::RoomListState state);
    void push_image_packs_updated();
    void push_account_prefs_updated(const std::string& json);
    void push_notification(const std::string& user_id,
                           const std::string& room_id, const std::string& room_name,
                           const std::string& sender, const std::string& body,
                           bool is_mention, std::vector<uint8_t> avatar_bytes);
    static void    on_login_clicked(GtkButton*, gpointer user_data);
    static void    on_back_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_verify_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data);
    void           on_send_clicked();
    void           toggle_emoji_picker();
    void           popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect);
    void           build_emoji_popover();
    void           build_sticker_popover();
    void           toggle_sticker_picker();
    void           build_join_room_dialog();
    void           open_join_room_dialog();
    void           build_sticker_context_menu();
public:
    void emoji_selected(const std::string& glyph);
    void apply_image_packs_updated();
private:
    static void    on_user_strip_right_click_(GtkGestureClick* gesture,
                                              int n_press, double x, double y,
                                              gpointer user_data);
    static void    on_user_strip_left_click_(GtkGestureClick* gesture,
                                             int n_press, double x, double y,
                                             gpointer user_data);
    static void    on_msg_right_click_(GtkGestureClick* gesture,
                                       int n_press, double x, double y,
                                       gpointer user_data);
    static void    on_sticker_save_activate_(GSimpleAction* action,
                                              GVariant* parameter,
                                              gpointer user_data);
    static void    on_logout_activate_(GSimpleAction* action,
                                       GVariant* parameter, gpointer user_data);
    static void    on_add_account_activate_(GSimpleAction* action,
                                            GVariant* parameter, gpointer user_data);
    static gboolean on_window_key_pressed_(GtkEventControllerKey*,
                                            guint keyval, guint,
                                            GdkModifierType,
                                            gpointer user_data);
    static gboolean on_window_close_request_(GtkWindow* window, gpointer user_data);

    void start_tray_if_needed_();

    // Multi-account management.
    void switch_active_account(int new_idx);
    void begin_add_account();
    void logout_active_account();
    void on_login_cancelled();
    void rebuild_account_picker();
    void open_account_picker(double anchor_x, double anchor_y);

    void show_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    void refresh_room_list();
    void on_room_selected(const std::string& room_id);
    void ensure_row_media(const tesseract::Event& ev);
    void clear_messages();
    void request_more_history(const std::string& room_id);
    void open_jump_to_date_dialog();
    static void on_jump_dialog_ok_(GtkButton*, gpointer user_data);
    static void on_jump_dialog_cancel_(GtkButton*, gpointer user_data);
    static void on_jump_dialog_destroy_(GtkWidget*, gpointer user_data);
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();
    void do_logout();
    void on_login_succeeded();
    void navigate_to_room(const std::string& room_id);
    void handle_notification(const std::string& user_id,
                              const std::string& room_id, const std::string& room_name,
                              const std::string& sender, const std::string& body,
                              bool is_mention, std::vector<uint8_t> avatar_bytes);
    void populate_user_strip();
    void maybe_show_recovery_banner();

    void ensure_sticker_image_async(std::string url);

    // ShellBase virtual hooks (GTK4 implementations).
    void post_to_ui_(std::function<void()> fn) override;
    void on_rooms_updated_() override;
    void on_media_bytes_ready_(const std::string& cache_key,
                                MediaKind kind,
                                std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                    const std::string& video_url) override;
    void on_url_preview_ready_(const std::string& url,
                                const tesseract::Client::UrlPreview& preview) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    std::unordered_map<std::string, tesseract::views::UrlPreviewData> url_preview_data_;

    void start_anim_tick_if_needed_();
    void invalidate_anim_consumers_();
    static gboolean on_tk_anim_tick_(gpointer user_data);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize  = tesseract::visual::kMsgAvatarSize;

    GtkApplication* app_              = nullptr;
    GtkWidget*      window_             = nullptr;
    GtkWidget*      content_stack_      = nullptr;
    GtkWidget*      main_content_       = nullptr;
    std::unique_ptr<LoginView> login_view_;
    GtkWidget*      room_nav_bar_       = nullptr;
    GtkWidget*      back_button_        = nullptr;
    GtkWidget*      space_name_lbl_     = nullptr;
    std::unique_ptr<tk::gtk4::Surface>            room_surface_;
    tesseract::views::RoomListView*               room_list_view_   = nullptr;
    std::unique_ptr<tk::NativeTextField>          room_search_field_;
    std::unique_ptr<tk::gtk4::Surface>            chat_surface_;
    tesseract::views::RoomView*                   room_view_         = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextArea>           room_text_area_;
    GtkWidget*      emoji_popover_      = nullptr;
    std::unique_ptr<tk::gtk4::Surface>      emoji_picker_surface_;
    tesseract::views::EmojiPicker*           emoji_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField>    emoji_picker_search_field_;
    std::string                             pending_reaction_event_id_;

    GtkWidget*      sticker_popover_      = nullptr;
    std::unique_ptr<tk::gtk4::Surface>      sticker_picker_surface_;
    tesseract::views::StickerPicker*        sticker_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField>    sticker_picker_search_field_;

    std::unique_ptr<tk::gtk4::Surface>       img_viewer_surface_;
    tesseract::views::ImageViewerOverlay*    img_viewer_ = nullptr;

    std::unique_ptr<tk::gtk4::Surface>       vid_viewer_surface_;
    tesseract::views::VideoViewerOverlay*    vid_viewer_ = nullptr;

    GtkWidget*      join_room_dialog_window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface>      join_room_surface_;
    tesseract::views::JoinRoomView*         join_room_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField>    join_room_alias_field_;

    GtkWidget*      sticker_ctx_menu_     = nullptr;
    GSimpleActionGroup* sticker_ctx_actions_ = nullptr;
    std::string     ctx_sticker_event_id_;
    std::string     ctx_sticker_mxc_url_;
    std::string     ctx_sticker_body_;
    std::string     ctx_sticker_info_json_;
    GtkWidget*      status_bar_         = nullptr;

    guint                    sync_status_debounce_id_ = 0;
    void                     refresh_sync_status();
    static gboolean          on_sync_status_debounce_(gpointer user_data);

    std::unique_ptr<tk::gtk4::Surface>      recovery_surface_;
    tesseract::views::RecoveryBanner*       recovery_shared_   = nullptr;

    std::unique_ptr<tk::gtk4::Surface>      verif_surface_;
    tesseract::views::VerificationBanner*   verif_shared_      = nullptr;
    std::unique_ptr<tk::NativeTextField>    recovery_key_field_;

    // Sidebar user identity strip.
    GtkWidget*      user_strip_       = nullptr;
    GtkWidget*      user_avatar_img_  = nullptr;
    GtkWidget*      user_name_lbl_    = nullptr;
    GtkWidget*      user_id_lbl_      = nullptr;  // Matrix ID second line
    GtkWidget*      user_popover_     = nullptr;

    // Account-picker popover (left-click, only when ≥2 accounts).
    GtkWidget*                                    account_picker_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface>            account_picker_surface_;
    tesseract::views::AccountPicker*              account_picker_         = nullptr;

    std::unique_ptr<LinuxGtkTrayIcon>     tray_;

    guint              tk_anim_tick_id_ = 0;

    guint        search_debounce_id_  = 0;
    guint        scroll_debounce_id_  = 0;
    std::string  search_pending_text_;
};

} // namespace gtk4
