#pragma once
#include <gtk/gtk.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>
#include "LinuxNotifier.h"
#include "LinuxGtkTrayIcon.h"

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/AccountPicker.h"
#include "views/ComposeBar.h"
#include "views/EmojiPicker.h"
#include "views/format.h"
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"
#include "views/StickerPicker.h"
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

/// Marshals SDK callbacks onto the GTK main loop via g_idle_add.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(GtkWindow* window) : window_(window) {}

    void set_user_id(std::string uid) { user_id_ = std::move(uid); }
    const std::string& user_id() const { return user_id_; }

    void on_timeline_reset(const std::string& room_id,
                            std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void on_message_inserted(const std::string& room_id,
                              std::size_t index,
                              std::unique_ptr<tesseract::Event> event) override;
    void on_message_updated(const std::string& room_id,
                             std::size_t index,
                             std::unique_ptr<tesseract::Event> event) override;
    void on_message_removed(const std::string& room_id,
                             std::size_t index) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;
    void on_room_list_state(tesseract::RoomListState state) override;
    void on_image_packs_updated() override;
    void on_account_prefs_updated(const std::string& json) override;
    void on_notification(const std::string& room_id, const std::string& room_name,
                         const std::string& sender,  const std::string& body,
                         bool is_mention) override;

    GtkWindow*  window_;
    std::string user_id_;
};

// ---------------------------------------------------------------------------

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();

    GtkWidget* widget() const { return window_; }

    void push_timeline_reset(std::string room_id,
                              std::vector<std::unique_ptr<tesseract::Event>> snapshot);
    void push_message_inserted(std::string room_id,
                                std::size_t index,
                                std::unique_ptr<tesseract::Event> ev);
    void push_message_updated(std::string room_id,
                               std::size_t index,
                               std::unique_ptr<tesseract::Event> ev);
    void push_message_removed(std::string room_id, std::size_t index);
    void push_paginate_result(std::string room_id, bool reached_start);
    void push_subscribe_result(std::string room_id, bool reached_start);
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
                           bool is_mention);

private:
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
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();
    void do_logout();
    void on_login_succeeded();
    void navigate_to_room(const std::string& room_id);
    void handle_notification(const std::string& user_id,
                              const std::string& room_id, const std::string& room_name,
                              const std::string& sender, const std::string& body,
                              bool is_mention);
    void populate_user_strip();
    void maybe_show_recovery_banner();

    tesseract::views::MessageRowData to_row_data(const tesseract::Event& ev);
    void ensure_room_avatar(const tesseract::RoomInfo& r);
    void ensure_user_avatar(const std::string& mxc);
    void ensure_media_image(const std::string& url, int max_w, int max_h);
    void ensure_reply_details(const std::string& event_id);
    void ensure_sticker_image_async(std::string url);

    enum class MediaKind : std::uint8_t {
        RoomAvatar,
        UserAvatar,
        MediaImage,
    };
    void request_room_avatar_async(const std::string& room_id,
                                     const std::string& mxc);
    void request_user_avatar_async(const std::string& mxc);
    void request_media_image_async(const std::string& url);
    std::unordered_set<std::string> media_fetches_in_flight_;

    void run_async_(std::function<void()> fn);

    std::atomic<bool>           shutting_down_{false};
    std::mutex                  workers_mu_;
    std::condition_variable     workers_cv_;
    int                         workers_in_flight_ = 0;

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
    GtkWidget*      room_header_        = nullptr;
    GtkWidget*      room_header_avatar_ = nullptr;
    GtkWidget*      room_header_name_   = nullptr;
    GtkWidget*      room_header_topic_  = nullptr;
    std::unique_ptr<tk::gtk4::Surface>            msg_surface_;
    tesseract::views::MessageListView*            message_list_view_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface>            compose_surface_;
    tesseract::views::ComposeBar*                  compose_shared_   = nullptr;
    std::unique_ptr<tk::NativeTextArea>            compose_text_area_;
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

    GtkWidget*      sticker_ctx_menu_     = nullptr;
    GSimpleActionGroup* sticker_ctx_actions_ = nullptr;
    std::string     ctx_sticker_event_id_;
    std::string     ctx_sticker_mxc_url_;
    std::string     ctx_sticker_body_;
    GtkWidget*      status_bar_         = nullptr;

    tesseract::RoomListState last_room_list_state_ = tesseract::RoomListState::Init;
    tesseract::BackupState   last_backup_state_    = tesseract::BackupState::Unknown;
    std::uint64_t            last_imported_keys_   = 0;
    guint                    sync_status_debounce_id_ = 0;
    bool                     sync_progress_shown_     = false;
    void                     refresh_sync_status();
    static gboolean          on_sync_status_debounce_(gpointer user_data);

    std::unique_ptr<tk::gtk4::Surface>      recovery_surface_;
    tesseract::views::RecoveryBanner*       recovery_shared_   = nullptr;
    std::unique_ptr<tk::NativeTextField>    recovery_key_field_;
    bool                                    recovery_banner_dismissed_ = false;

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

    // Multi-account state. client_ and event_handler_ are non-owning
    // aliases of accounts_[active_account_index_]->client / ->bridge,
    // repointed by switch_active_account().
    std::vector<std::unique_ptr<tesseract::AccountSession>> accounts_;
    int              active_account_index_ = -1;
    tesseract::Client*  client_        = nullptr;   // non-owning alias
    EventHandler*       event_handler_ = nullptr;   // non-owning alias

    // Per-account room snapshot cache, keyed by user_id.
    std::unordered_map<std::string, std::vector<tesseract::RoomInfo>> per_account_rooms_;

    // In-flight login (OAuth into a temp dir; rename on success).
    std::unique_ptr<tesseract::Client> pending_login_client_;
    std::filesystem::path              pending_login_temp_dir_;
    bool                               pending_login_is_add_account_ = false;
    int                                add_account_return_idx_       = -1;

    // Cached identity for the active account (repopulated by switch_active_account).
    std::string     my_user_id_;
    std::string     my_display_name_;
    std::string     my_avatar_url_;

    std::unique_ptr<LinuxGtkTrayIcon>     tray_;
    std::vector<tesseract::RoomInfo>  rooms_;
    std::string                    current_room_id_;
    std::string                    pending_restore_room_;
    std::unordered_map<std::string, std::vector<uint8_t>> avatar_cache_;

    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;

    std::unordered_set<std::string>                              voice_prefetched_;
    std::unordered_set<std::string> video_thumb_in_flight_;

    struct AnimatedImage {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                         delays_ms;
        std::size_t                              current        = 0;
        std::int64_t                             next_advance_ms = 0;
    };
    std::unordered_map<std::string, AnimatedImage>               tk_anim_images_;
    guint                                                         tk_anim_tick_id_ = 0;

    std::unordered_set<std::string>                              sticker_fetches_in_flight_;

    guint        search_debounce_id_  = 0;
    std::string  search_pending_text_;

    std::unordered_set<std::string>                              reply_details_requested_;

    std::vector<std::string>                               space_stack_;

    struct PaginationState { bool in_flight = false; bool reached_start = false; };
    std::unordered_map<std::string, PaginationState> pagination_;
    static constexpr std::uint16_t kPaginationBatch = 50;
};

} // namespace gtk4
