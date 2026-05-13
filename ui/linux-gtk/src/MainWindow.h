#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/visual.h>

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/ComposeBar.h"
#include "views/EmojiPicker.h"
#include "views/format.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"
#include "views/StickerPicker.h"

#include <memory>
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
    void on_image_packs_updated() override;

    GtkWindow* window_;
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
    void push_rooms(std::vector<tesseract::RoomInfo> rooms);
    void push_error(std::string description);
    void handle_reconnect();
    void handle_auth_error(bool soft_logout);
    void push_backup_progress(tesseract::BackupProgress progress);
    void push_image_packs_updated();

private:
    static void    on_login_clicked(GtkButton*, gpointer user_data);
    static void    on_back_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_verify_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data);
    void           on_send_clicked();
    void           toggle_emoji_picker();
    /// Open the emoji popover anchored to a sub-rect of `parent` (rect is
    /// in `parent`'s local widget coords). Used for the reaction "+" chip.
    void           popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect);
    void           build_emoji_popover();
    void           build_sticker_popover();
    void           toggle_sticker_picker();
    void           build_sticker_context_menu();
public:
    // Reached from the shared EmojiPicker's on_selected callback.
    void emoji_selected(const std::string& glyph);
    // Reached from the EventHandler when the SDK rebuilds the image-pack
    // cache (sync delivers a relevant event, or a user-pack write lands).
    void apply_image_packs_updated();
private:
    static void    on_user_strip_right_click_(GtkGestureClick* gesture,
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

    void show_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    void refresh_room_list();
    void on_room_selected(const std::string& room_id);
    // Resolve any media bytes the row references and decode them into
    // tk::Images held in `tk_avatars_` / `tk_images_`. Shared by every
    // positional-callback path (insert / update / reset).
    void ensure_row_media(const tesseract::Event& ev);
    void clear_messages();
    /// Kick off back-pagination for `room_id` on a worker thread. Hooked
    /// to `MessageListView::on_near_top`; guarded by `pagination_` state.
    void request_more_history(const std::string& room_id);
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();
    void do_logout();
    void on_login_succeeded();
    void populate_user_strip();
    void maybe_show_recovery_banner();

    // Convert a polymorphic SDK Event into the flat MessageRowData the
    // shared MessageListView consumes; downloads referenced media bytes
    // on demand and stashes decoded tk::Images in tk_images_.
    tesseract::views::MessageRowData to_row_data(const tesseract::Event& ev);
    void ensure_room_avatar(const tesseract::RoomInfo& r);
    void ensure_user_avatar(const std::string& mxc);
    void ensure_media_image(const std::string& url, int max_w, int max_h);
    void ensure_reply_details(const std::string& event_id);

    /// Background-thread variant for the sticker picker: kick off a
    /// `fetch_source_bytes` on a detached thread, then post the decoded
    /// `tk::Image` back to the UI thread via `g_idle_add`. Deduplicates
    /// via `sticker_fetches_in_flight_` so each cell paint doesn't spawn
    /// a duplicate fetch.
    void ensure_sticker_image_async(std::string url);

    /// Async media fetches for room avatars, user avatars, and inline
    /// media. The synchronous Rust FFI does a `tokio::block_on` per
    /// call; running it on the UI thread freezes the GTK main loop on
    /// large accounts (one round-trip per room avatar serialised on
    /// first sync). These helpers spawn a detached worker and bounce
    /// the bytes back through `g_idle_add` for decode + cache.
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

    /// Lazily install a 16 ms `g_timeout` that drives `tk_anim_tick_`.
    /// No-op when the tick is already armed.
    void start_anim_tick_if_needed_();
    /// Repaint every surface that reads from `tk_anim_images_` after a
    /// frame advance (or on initial population of an entry). Cheap.
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
    tesseract::views::RoomListView*               room_list_view_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>          room_search_field_;
    GtkWidget*      room_header_        = nullptr;
    GtkWidget*      room_header_avatar_ = nullptr;
    GtkWidget*      room_header_name_   = nullptr;
    GtkWidget*      room_header_topic_  = nullptr;
    std::unique_ptr<tk::gtk4::Surface>            msg_surface_;
    tesseract::views::MessageListView*            message_list_view_ = nullptr; // borrowed
    // Compose bar — tk::gtk4::Surface hosting the shared ComposeBar; the
    // text input is a NativeTextArea overlaid on the bar's text_area_rect.
    std::unique_ptr<tk::gtk4::Surface>            compose_surface_;
    tesseract::views::ComposeBar*                  compose_shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextArea>            compose_text_area_;
    GtkWidget*      emoji_popover_      = nullptr;
    std::unique_ptr<tk::gtk4::Surface>      emoji_picker_surface_;
    tesseract::views::EmojiPicker*           emoji_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>    emoji_picker_search_field_;
    // When set, the next emoji selection routes through send_reaction
    // for this event_id rather than inserting into the compose bar.
    std::string                             pending_reaction_event_id_;

    // Sticker picker — parallel to the emoji picker. Popup-style
    // GtkPopover hosting a tk::gtk4::Surface that paints the shared
    // tesseract::views::StickerPicker.
    GtkWidget*      sticker_popover_      = nullptr;
    std::unique_ptr<tk::gtk4::Surface>      sticker_picker_surface_;
    tesseract::views::StickerPicker*        sticker_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>    sticker_picker_search_field_;

    // Right-click context menu on the message surface — shown when a
    // right-click lands on a sticker that isn't yet in the user's
    // Saved Stickers pack. Built once; pointed_to + activated per-click.
    GtkWidget*      sticker_ctx_menu_     = nullptr;
    GSimpleActionGroup* sticker_ctx_actions_ = nullptr;
    // Captured at right-click time so the action handler can read them
    // without holding a pointer into MessageListView's per-frame
    // sticker_geom_ map.
    std::string     ctx_sticker_event_id_;
    std::string     ctx_sticker_mxc_url_;
    std::string     ctx_sticker_body_;
    GtkWidget*      status_bar_         = nullptr;

    // Recovery banner — shared widget hosted in a tk::gtk4::Surface.
    // Visibility is toggled at the Surface widget level; the password
    // field is a NativeTextField overlay (GtkEntry under the hood).
    std::unique_ptr<tk::gtk4::Surface>      recovery_surface_;
    tesseract::views::RecoveryBanner*       recovery_shared_   = nullptr;
    std::unique_ptr<tk::NativeTextField>    recovery_key_field_;
    bool                                    recovery_banner_dismissed_ = false;

    GtkWidget*      user_strip_       = nullptr;
    GtkWidget*      user_avatar_img_  = nullptr;
    GtkWidget*      user_name_lbl_    = nullptr;
    GtkWidget*      user_popover_     = nullptr;
    std::string     my_display_name_;
    std::string     my_avatar_url_;

    tesseract::Client              client_;
    std::unique_ptr<EventHandler>  event_handler_;
    std::vector<tesseract::RoomInfo>  rooms_;
    std::string                    current_room_id_;
    std::string                    pending_restore_room_;
    std::string                    my_user_id_;
    // Raw bytes-cache for the room-header avatar (still painted via
    // GdkPixbuf into a GtkImage). Sidebar + message-list avatars and
    // inline media go through tk_avatars_ / tk_images_ below.
    std::unordered_map<std::string, std::vector<uint8_t>> avatar_cache_;

    // tk::Image caches mirror the QPixmap pattern from the Qt6 port —
    // populated alongside SDK fetch_*_bytes calls, read back by the
    // shared RoomListView / MessageListView provider lambdas.
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;

    // Voice-message source tokens already prefetched (or in flight).
    // The SDK's media cache holds the bytes; we just remember which we've
    // dispatched workers for to dedupe.
    std::unordered_set<std::string>                              voice_prefetched_;

    /// Animated inline-media entries (GIF / animated WebP / APNG). Same
    /// shape as the Qt port's `AnimatedImage`. Decoded eagerly: each
    /// frame is a fully-baked cairo surface wrapped in `tk::Image`. The
    /// `tk_anim_tick_id_` g_timeout drives `tk_anim_tick_` and repaints
    /// `msg_surface_` + `sticker_picker_surface_` when frames advance.
    struct AnimatedImage {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                         delays_ms;
        std::size_t                              current        = 0;
        std::int64_t                             next_advance_ms = 0;
    };
    std::unordered_map<std::string, AnimatedImage>               tk_anim_images_;
    guint                                                         tk_anim_tick_id_ = 0;

    // URLs for which `ensure_sticker_image_async` has spawned a worker
    // that hasn't yet posted its result. Stops every cell-paint from
    // queueing a duplicate fetch.
    std::unordered_set<std::string>                              sticker_fetches_in_flight_;

    // Replied-to event IDs for which we have already called
    // fetch_reply_details this subscription session. Cleared on room switch.
    std::unordered_set<std::string>                              reply_details_requested_;

    std::vector<std::string>                               space_stack_;

    // Per-room back-pagination state, keyed by room ID. See the matching
    // struct in the Qt shell — same semantics.
    struct PaginationState { bool in_flight = false; bool reached_start = false; };
    std::unordered_map<std::string, PaginationState> pagination_;
    static constexpr std::uint16_t kPaginationBatch = 50;
};

} // namespace gtk4
