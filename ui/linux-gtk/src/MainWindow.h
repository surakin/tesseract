#pragma once
#include <gtk/gtk.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>
#include "GtkSniTrayIcon.h"

#include "app/AccountManager.h"
#include "app/EventHandlerBase.h"
#include "app/SettingsController.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/AccountPicker.h"
#include "views/ComposePopups.h"
#include "views/EmojiPicker.h"
#include "views/ImageViewerOverlay.h"
#include "views/MainAppWidget.h"
#include "views/VideoViewerOverlay.h"
#include "views/StickerPicker.h"
#include "views/JoinRoomView.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gtk4
{

class LoginView;
class SettingsWidget;

// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase
{
public:
    explicit MainWindow(tesseract::AccountManager& account_manager, GtkApplication* app);
    ~MainWindow();

    GtkWidget* widget() const
    {
        return window_;
    }
    // The GtkApplication this shell belongs to. Pop-out windows associate
    // themselves with it (gtk_window_set_application) so they are proper
    // application windows — without this a bare gtk_window_new() pop-out routes
    // popover keyboard grabs incorrectly (composer input dies while a popup is
    // open).
    GtkApplication* application() const
    {
        return app_;
    }
    void present()
    {
        gtk_window_present(GTK_WINDOW(window_));
    }

    // Bring the main window forward and open the quick switcher. Pop-out room
    // windows route here on Ctrl+K — their own shortcut controller is scoped
    // to the pop-out window, while the switcher widget lives in the main one.
    void request_quick_switch_from_popout()
    {
        present();
        open_quick_switch_();
    }

    // These are called from internal async callbacks (paginate/subscribe workers).
    void push_paginate_result(std::string room_id, bool reached_start);

private:
    // ── EventHandlerBase UI-thread hook overrides (GTK4) ──────────────────────
    // timeline-reset + message insert/update/remove are now concrete in
    // ShellBase (they drive room_view_ + request_relayout_ directly).
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void refresh_user_strip_() override;
    void request_relogin_(const std::string& user_id) override;
    void
    handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void refresh_pickers_packs_() override;
    void handle_verification_request_ui_(std::string flow_id,
                                         std::string user_id,
                                         std::string device_id,
                                         bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(std::string flow_id,
                                           std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_notification_ui_(std::string user_id, std::string room_id,
                                 std::string room_name, std::string sender,
                                 std::string body, bool is_mention,
                                 std::vector<uint8_t> avatar_bytes,
                                 std::vector<uint8_t> image_bytes) override;
    void on_room_list_state_ui_() override;
    void on_inflight_ui_() override;
    void on_server_info_ready_ui_() override;
    void on_own_extended_profile_ready_ui_() override;
    void on_profile_field_result_ui_(const std::string& key, bool ok,
                                     const std::string& error) override;
    void draw_inflight_dot_(cairo_t* cr);
    void update_typing_bar_(const std::string& text, bool visible) override;
    void on_show_status_message_ui_(const std::string& msg) override;
    void on_restore_status_ui_() override;
    std::vector<tk::Rect> get_screen_work_areas_() const override;

    // user_id identifies which account's snapshot this is (for caching).
    void push_rooms(std::string user_id,
                    std::vector<tesseract::RoomInfo> rooms);
    void push_error(std::string description);
    void push_backup_progress(tesseract::BackupProgress progress);
    void push_room_list_state(tesseract::RoomListState state);
    void push_notification(const std::string& user_id,
                           const std::string& room_id,
                           const std::string& room_name,
                           const std::string& sender, const std::string& body,
                           bool is_mention, std::vector<uint8_t> avatar_bytes,
                           std::vector<uint8_t> image_bytes);
    static void on_login_clicked(GtkButton*, gpointer user_data);
    void on_send_clicked();
    void toggle_emoji_picker();
    void popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect);
    void popup_sticker_at_rect(GtkWidget* parent, tk::Rect local_rect);
    void build_emoji_popover();
    void build_sticker_popover();
    void toggle_sticker_picker();
    void build_join_room_dialog();
    void open_join_room_dialog();
    void build_sticker_context_menu();

public:
    void emoji_selected(const std::string& glyph);
    void emoticon_selected(const tesseract::ImagePackImage& img);

private:
    static void on_msg_right_click_(GtkGestureClick* gesture, int n_press,
                                    double x, double y, gpointer user_data);
    static void on_sticker_save_activate_(GSimpleAction* action,
                                          GVariant* parameter,
                                          gpointer user_data);
    static void on_copy_action_(GSimpleAction* action, GVariant* parameter,
                                gpointer user_data);
    void open_settings_();

    // Ctrl+K quick switcher — open focuses the native search field; close
    // hides it and relayouts.
    void open_quick_switch_();
    void close_quick_switch_();

    // Ctrl+Shift+F global message search — open focuses the native search
    // field; close hides it and relayouts.
    void open_message_search_();
    void close_message_search_();
    void close_forward_picker_();
    void focus_forward_picker_field_() override;
    void hide_forward_picker_field_() override;
    // Ctrl+F per-room "find in conversation" search bar.
    void open_find_in_room_();
    void close_find_in_room_();

    static gboolean on_window_key_pressed_(GtkEventControllerKey*, guint keyval,
                                           guint, GdkModifierType,
                                           gpointer user_data);
    // Global-scope Ctrl+K shortcut callback — opens the quick switcher even
    // while a native entry / text view holds focus.
    static gboolean on_quick_switch_shortcut_(GtkWidget*, GVariant*,
                                              gpointer user_data);
    // Global-scope Ctrl+Shift+F shortcut callback — opens message search.
    static gboolean on_message_search_shortcut_(GtkWidget*, GVariant*,
                                                gpointer user_data);
    // Global-scope Ctrl+F shortcut callback — opens per-room find bar.
    static gboolean on_find_in_room_shortcut_(GtkWidget*, GVariant*,
                                              gpointer user_data);
    // Global-scope Alt+Left / Alt+Right shortcut callbacks — room history nav.
    static gboolean on_nav_back_shortcut_(GtkWidget*, GVariant*,
                                          gpointer user_data);
    static gboolean on_nav_fwd_shortcut_(GtkWidget*, GVariant*,
                                         gpointer user_data);
    static gboolean on_window_close_request_(GtkWindow* window,
                                             gpointer user_data);

    void start_tray_if_needed_();

    // Multi-account management.
    void switch_active_account(const std::string& user_id);
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
    // Bind the UI to the now-active account `uid` and finish startup (settings
    // controller, status, main surface, tray). Shared by the cold restore path
    // and the secondary-window bind path in do_login().
    void finish_login_ui_(const std::string& uid);
    void do_logout();
    void on_login_succeeded();
    void wire_key_dialog_callbacks_();
    void navigate_to_room(const std::string& room_id);
    void handle_notification(const std::string& user_id,
                             const std::string& room_id,
                             const std::string& room_name,
                             const std::string& sender, const std::string& body,
                             bool is_mention, std::vector<uint8_t> avatar_bytes,
                             std::vector<uint8_t> image_bytes);
    void populate_user_strip();

    // ShellBase virtual hooks (GTK4 implementations).
    bool is_room_search_active_() const override
    {
        return !search_pending_text_.empty();
    }
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void apply_theme_ui_(const tk::Theme& t) override;
    tk::ThemeMode os_color_scheme_() const override;
    void post_to_ui_(std::function<void()> fn) override;
    void post_to_ui_after_(int ms, std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void on_rooms_updated_() override;
    void on_invites_updated_() override;
    void on_space_children_cache_ready_ui_() override;
    void on_space_unjoined_summaries_ready_ui_(const std::string&) override;
    void on_join_room_outcome_ui_(bool ok, const std::string& room_id) override;
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
    void show_qr_grant_overlay_() override;
    void hide_qr_grant_overlay_() override;
    void open_join_room_dialog_ui_(const std::string& prefill) override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;
    void on_media_bytes_ready_(const std::string& cache_key, MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override;
    void bind_settings_controller_() override;
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void repaint_pickers_() override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    float get_message_scroll_fraction_() override;
    void set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_() override;
    void set_compose_draft_(const std::string&) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;

    // Extract media metadata from a dropped file on a background thread, then
    // post via g_idle_add. When `target` is non-null the result goes to that
    // compose bar (a pop-out window's), guarded by `target_alive`; otherwise to
    // the main window's. Overrides the ShellBase drag-and-drop probe hook.
    void extract_drop_media_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             std::string mime,
                             tesseract::views::ComposeBar* target = nullptr,
                             std::shared_ptr<bool> target_alive = nullptr)
        override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void raise_and_activate_() override;
    void rebuild_tray_() override;
    bool is_ctrl_held_() const override;
    void switch_active_account_(const std::string& user_id) override;
    void refresh_account_ui_after_switch_() override;
    void spawn_main_window_(
        std::shared_ptr<tesseract::AccountSession> account) override;
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string& uid) override;
    void install_account_notifier_(
        tesseract::AccountSession& session) override;
    void install_account_up_connector_(
        tesseract::AccountSession& session) override;
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override;
    tesseract::CallWindowBase* create_call_window_() override;
#endif

    void start_anim_tick_if_needed_();
    void invalidate_anim_consumers_();
    static gboolean on_tk_anim_tick_(gpointer user_data);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* content_stack_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> branding_surface_;
    std::unique_ptr<LoginView> login_view_;
    std::unique_ptr<SettingsWidget> settings_widget_;

    // Single surface hosting the full main-app widget tree.
    // main_app_ / room_view_ live in ShellBase (assigned in the constructor).
    std::unique_ptr<tk::gtk4::Surface> main_app_surface_;

    // Borrowed pointers into main_app_ (extracted in constructor).
    tesseract::views::RoomListView* room_list_view_ = nullptr;
    std::unique_ptr<tk::NativeTextField> room_search_field_;
    std::unique_ptr<tk::NativeTextField> quick_switch_field_;
    std::unique_ptr<tk::NativeTextField> message_search_field_;
    std::unique_ptr<tk::NativeTextField> forward_picker_field_;
    std::unique_ptr<tk::NativeTextField> find_in_room_field_;
    std::unique_ptr<tk::NativeTextArea> room_text_area_;
    std::unique_ptr<tk::NativeTextArea> topic_text_area_;
    GtkWidget* emoji_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> emoji_picker_surface_;
    tesseract::views::EmojiPicker* emoji_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> emoji_picker_search_field_;
    std::string pending_reaction_event_id_;

    GtkWidget* sticker_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> sticker_picker_surface_;
    tesseract::views::StickerPicker* sticker_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> sticker_picker_search_field_;

    // ── Shortcode popup ───────────────────────────────────────────────────
    GtkWidget* shortcode_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> shortcode_popup_surface_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    void show_shortcode_popup_(tk::Rect cursor_local, int rows);
    void hide_shortcode_popup_();
    bool shortcode_popup_visible_() const
    {
        return shortcode_popover_ && gtk_widget_get_visible(shortcode_popover_);
    }

    // ── Slash-command popup ───────────────────────────────────────────────
    GtkWidget* slash_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> slash_popup_surface_;
    tesseract::views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    void show_slash_popup_(tk::Rect cursor_local, int rows);
    void hide_slash_popup_();
    bool slash_popup_visible_() const
    {
        return slash_popover_ && gtk_widget_get_visible(slash_popover_);
    }

    // ── GIF picker (/gif <query>) ────────────────────────────────────────────
    GtkWidget* gif_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> gif_popup_surface_;
    tesseract::views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> gif_previews_;
    std::unordered_set<std::string> gif_preview_inflight_;
    std::unordered_set<std::string> gif_anim_inflight_;
    std::shared_ptr<bool> gif_alive_ = std::make_shared<bool>(true);
    // Two-stage GIF strip cell provider (body parameterised on a repaint
    // callback). Shared by this window's strip and every pop-out's via the
    // gif_strip_image_ override.
    std::function<const tk::Image*(const tesseract::GifResult&,
                                   const std::function<void()>&)>
        gif_strip_provider_;
    const tk::Image*
    gif_strip_image_(const tesseract::GifResult& result,
                     const std::function<void()>& repaint) override;
    void show_gif_popup_();
    void hide_gif_popup_();
    bool gif_popup_visible_() const
    {
        return gif_popover_ && gtk_widget_get_visible(gif_popover_);
    }
    void handle_gif_results_ui_(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results) override;
    void handle_gif_search_failed_ui_(std::uint64_t request_id,
                                      std::string message) override;

    // ── @mention popup ────────────────────────────────────────────────────
    // cached_room_members_ is the room-switch member prefetch used by the
    // received-mention-pill avatar provider; the MentionController fetches its
    // own member list independently for autocomplete.
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;

    GtkWidget* mention_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    void show_mention_popup_(tk::Rect cursor_local, int rows);
    void hide_mention_popup_();
    bool mention_popup_visible_() const
    {
        return mention_popover_ && gtk_widget_get_visible(mention_popover_);
    }

    GtkWidget* topic_tooltip_popover_ = nullptr;
    GtkWidget* topic_tooltip_label_ = nullptr;

    tesseract::views::ImageViewerOverlay* img_viewer_ = nullptr;

    tesseract::views::VideoViewerOverlay* vid_viewer_ = nullptr;

    GtkWidget* join_room_dialog_window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> join_room_surface_;
    tesseract::views::JoinRoomView* join_room_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> join_room_alias_field_;
    uint32_t join_room_gen_ = 0; // guards stale async callbacks

    GtkWidget* sticker_ctx_menu_ = nullptr;
    GSimpleActionGroup* sticker_ctx_actions_ = nullptr;
    std::string ctx_sticker_event_id_;

    GtkWidget* copy_ctx_menu_ = nullptr;
    GSimpleActionGroup* copy_ctx_actions_ = nullptr;
    void build_copy_context_menu_();
    std::string ctx_sticker_mxc_url_;
    std::string ctx_sticker_body_;
    std::string ctx_sticker_info_json_;
    GtkWidget* status_bar_ = nullptr;
    GtkWidget* inflight_dot_ = nullptr;

    guint sync_status_debounce_id_ = 0;
    guint mark_read_timer_id_ = 0;
    void refresh_sync_status();
    static gboolean on_sync_status_debounce_(gpointer user_data);

    tesseract::views::VerificationBanner* verif_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> enc_passphrase_field_;
    std::unique_ptr<tk::NativeTextField> enc_key_field_;
    std::unique_ptr<tk::NativeTextField> qr_check_code_field_;

    GtkWidget*       user_popover_      = nullptr;

    // Account-picker popover (left-click, only when ≥2 accounts).
    GtkWidget* account_picker_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> account_picker_surface_;
    tesseract::views::AccountPicker* account_picker_ = nullptr;

    GtkCssProvider* theme_css_provider_ = nullptr;
    gulong prefer_dark_notify_id_ = 0;

    std::unique_ptr<GtkSniTrayIcon> tray_;

    guint tk_anim_tick_id_ = 0;
    guint presence_tick_id_ = 0;

    guint scroll_debounce_id_ = 0;
    std::string search_pending_text_;

    // Liveness sentinel for one-shot g_timeout_add payloads (verification
    // auto-hide, sync reconnect). These can outnumber a single tracked
    // source id and must not fire on a destroyed `this`; payloads hold a
    // weak_ptr and bail if it has expired.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace gtk4
