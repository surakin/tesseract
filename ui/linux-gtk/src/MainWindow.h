#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/visual.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gtk4 {

class LoginView;

/// Marshals SDK callbacks onto the GTK main loop via g_idle_add.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(GtkWindow* window) : window_(window) {}

    void on_message(tesseract::Event* ev) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_timeline_reset(const std::string& room_id) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;

    GtkWindow* window_;
};

// ---------------------------------------------------------------------------

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();

    GtkWidget* widget() const { return window_; }

    void push_event(std::unique_ptr<tesseract::Event> ev);
    void push_rooms(std::vector<tesseract::RoomInfo> rooms);
    void push_error(std::string description);
    void handle_reconnect();
    void handle_auth_error(bool soft_logout);
    void push_timeline_reset(std::string room_id);
    void push_backup_progress(tesseract::BackupProgress progress);

private:
    static void    on_send_clicked(GtkButton*, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey* controller,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, gpointer user_data);
    static void    on_room_row_activated(GtkListBox*, GtkListBoxRow*, gpointer user_data);
    static void    on_login_clicked(GtkButton*, gpointer user_data);
    static void    on_adj_upper_changed_(GObject* obj, GParamSpec*, gpointer user_data);
    static void    on_back_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_verify_clicked_(GtkButton*, gpointer user_data);
    static void    on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data);
    static void    on_user_strip_right_click_(GtkGestureClick* gesture,
                                              int n_press, double x, double y,
                                              gpointer user_data);
    static void    on_logout_activate_(GSimpleAction* action,
                                       GVariant* parameter, gpointer user_data);
    static void    on_reaction_clicked_(GtkButton* btn, gpointer user_data);
    static void    on_message_right_click_(GtkGestureClick* gesture,
                                           int n_press, double x, double y,
                                           gpointer user_data);
    static void    on_message_delete_clicked_(GtkButton* btn, gpointer user_data);
    static void    on_message_delete_confirm_(GObject* source,
                                              GAsyncResult* result,
                                              gpointer user_data);
    void           perform_redact(const std::string& room_id,
                                  const std::string& event_id);

    /// Build a footer GtkBox (horizontal) holding reaction chips on the left
    /// and the timestamp anchored to the right. Always returned non-null; an
    /// event with no reactions and no timestamp still yields an empty footer
    /// so the in-place rebuild path has a uniform widget to swap in.
    GtkWidget* build_message_footer(const tesseract::Event& ev,
                                    const std::string& ts_str);

    void show_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    void refresh_room_list();
    void append_event(const tesseract::Event& ev);
    void clear_messages();
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();
    void do_logout();
    void on_login_succeeded();
    void populate_user_strip();
    void maybe_show_recovery_banner();

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
    GtkWidget*      room_list_          = nullptr;
    GtkWidget*      room_header_        = nullptr;
    GtkWidget*      room_header_avatar_ = nullptr;
    GtkWidget*      room_header_name_   = nullptr;
    GtkWidget*      room_header_topic_  = nullptr;
    GtkWidget*      msg_scroll_         = nullptr;
    GtkWidget*      msg_box_            = nullptr;
    GtkWidget*      input_text_view_    = nullptr;
    GtkWidget*      send_btn_           = nullptr;
    GtkWidget*      status_bar_         = nullptr;    GtkWidget*      recovery_banner_       = nullptr;
    GtkWidget*      recovery_label_        = nullptr;
    GtkWidget*      recovery_key_entry_    = nullptr;
    GtkWidget*      recovery_verify_btn_   = nullptr;
    bool            recovery_banner_dismissed_ = false;

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
    std::string                    my_user_id_;
    std::unordered_map<std::string, std::vector<uint8_t>> avatar_cache_;
    std::unordered_map<std::string, std::vector<uint8_t>> user_avatar_cache_;
    std::unordered_map<std::string, std::vector<uint8_t>> image_cache_;
    std::unordered_map<std::string, GtkWidget*>           msg_event_widgets_;
    bool                                                   auto_scroll_pending_ = false;
    std::vector<std::string>                               space_stack_;
};

} // namespace gtk4
