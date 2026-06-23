#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include "app/CallWindowBase.h"
#include "tk/host_gtk.h"
#include <gtk/gtk.h>
#include <memory>

namespace gtk4 { class MainWindow; }

namespace gtk4
{

// Secondary call pop-out window for the GTK4 shell.
// Hosts a tk::gtk4::Surface whose root is a CallOverlayWidget in Popout mode.
class CallWindow : public tesseract::CallWindowBase
{
public:
    explicit CallWindow(MainWindow* parent_shell);
    ~CallWindow() override;

    void bring_to_front()               override;
    void close_window()                 override;
    void apply_theme(const tk::Theme&)  override;
    void request_relayout()             override;
    void request_repaint()              override;

private:
    static void on_destroy_(GtkWidget* widget, gpointer self);

    GtkWindow*                           window_  = nullptr;
    std::unique_ptr<tk::gtk4::Surface>   surface_;
};

} // namespace gtk4
#endif // TESSERACT_CALLS_ENABLED
