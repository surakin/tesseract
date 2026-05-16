#pragma once
#include <gtk/gtk.h>
#include "app/RoomWindowBase.h"
#include "tk/host_gtk.h"
#include <memory>

namespace gtk4 { class MainWindow; }

namespace gtk4 {

// A secondary (pop-out) room window for the GTK4 shell.
class RoomWindow : public tesseract::RoomWindowBase {
public:
    RoomWindow(MainWindow* parent_shell, const std::string& room_id);
    ~RoomWindow() override;

    void bring_to_front()   override;
    void close_window()     override;
    void request_relayout() override;

private:
    static void on_destroy_(GtkWidget* widget, gpointer self);

    MainWindow*                     parent_shell_;
    GtkWindow*                      window_  = nullptr;
    std::unique_ptr<tk::gtk4::Surface> surface_;
};

} // namespace gtk4
