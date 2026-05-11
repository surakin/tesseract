#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gtk4 {

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

private:
    static void    on_send_clicked(GtkButton*, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey* controller,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, gpointer user_data);
    static void    on_room_row_activated(GtkListBox*, GtkListBoxRow*, gpointer user_data);
    static void    on_login_clicked(GtkButton*, gpointer user_data);

    void populate_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    void append_event(const tesseract::Event& ev);
    void clear_messages();
    void update_room_header(const tesseract::RoomInfo& info);
    void do_login();

    static constexpr int kRoomAvatarSize = 36;
    static constexpr int kMsgAvatarSize  = 32;

    GtkApplication* app_              = nullptr;
    GtkWidget*      window_             = nullptr;
    GtkWidget*      room_list_          = nullptr;
    GtkWidget*      room_header_        = nullptr;
    GtkWidget*      room_header_avatar_ = nullptr;
    GtkWidget*      room_header_name_   = nullptr;
    GtkWidget*      room_header_topic_  = nullptr;
    GtkWidget*      msg_scroll_         = nullptr;
    GtkWidget*      msg_box_            = nullptr;
    GtkWidget*      input_text_view_    = nullptr;
    GtkWidget*      send_btn_           = nullptr;
    GtkWidget*      status_bar_         = nullptr;

    tesseract::Client              client_;
    std::unique_ptr<EventHandler>  event_handler_;
    std::vector<tesseract::RoomInfo>  rooms_;
    std::string                    current_room_id_;
    std::string                    my_user_id_;
    std::unordered_map<std::string, std::vector<uint8_t>> avatar_cache_;
    std::unordered_map<std::string, std::vector<uint8_t>> user_avatar_cache_;
    std::unordered_map<std::string, std::vector<uint8_t>> image_cache_;
    std::unordered_map<std::string, GtkWidget*>           msg_event_widgets_;
};

} // namespace gtk4
