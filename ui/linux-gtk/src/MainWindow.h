#pragma once
#include <gtk/gtk.h>

#include <tesseract/client.hpp>
#include <tesseract/event_handler.hpp>

#include <memory>
#include <string>
#include <vector>

namespace gtk4 {

/// Marshals SDK callbacks onto the GTK main loop via g_idle_add.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(GtkWindow* window) : window_(window) {}

    void on_message(const tesseract::Message& msg) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description) override;
    void on_session_saved(const std::string& session_json) override;

    // Non-owning reference; caller manages lifetime.
    GtkWindow* window_;
};

// ---------------------------------------------------------------------------

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();

    GtkWidget* widget() const { return window_; }

    // Called from EventHandler via g_idle_add.
    void push_message(tesseract::Message msg);
    void push_rooms(std::vector<tesseract::RoomInfo> rooms);
    void push_error(std::string description);

private:
    static void on_send_clicked(GtkButton*, gpointer user_data);
    static void on_room_row_activated(GtkListBox*, GtkListBoxRow*, gpointer user_data);
    static void on_login_clicked(GtkButton*, gpointer user_data);

    void populate_rooms(const std::vector<tesseract::RoomInfo>& rooms);
    void append_message(const std::string& sender, const std::string& body);
    void do_login();

    GtkApplication* app_    = nullptr;
    GtkWidget* window_      = nullptr;
    GtkWidget* room_list_   = nullptr;
    GtkWidget* msg_view_    = nullptr;
    GtkWidget* msg_scroll_  = nullptr;
    GtkWidget* input_entry_ = nullptr;
    GtkWidget* send_btn_    = nullptr;
    GtkWidget* status_bar_  = nullptr;

    tesseract::Client             client_;
    std::unique_ptr<EventHandler>    event_handler_;
    std::vector<tesseract::RoomInfo>    rooms_;
    std::string                      current_room_id_;
};

} // namespace gtk4
