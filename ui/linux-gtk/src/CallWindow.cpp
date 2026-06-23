#ifdef TESSERACT_CALLS_ENABLED
#include "CallWindow.h"
#include "MainWindow.h"

#include "views/CallOverlayWidget.h"

namespace gtk4
{

CallWindow::CallWindow(MainWindow* parent_shell)
    : tesseract::CallWindowBase(parent_shell)
{
    window_ = GTK_WINDOW(gtk_window_new());

    // Associate with the GtkApplication so this is a proper application window,
    // matching the approach used by RoomWindow to avoid keyboard-grab issues.
    if (parent_shell && parent_shell->application())
        gtk_window_set_application(window_, parent_shell->application());

    gtk_window_set_title(window_, "Call");
    gtk_window_set_default_size(window_, 640, 480);

    surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    gtk_window_set_child(window_, surface_->widget());

    auto overlay = std::make_unique<tesseract::views::CallOverlayWidget>();
    call_overlay_widget_ = overlay.get();
    surface_->set_root(std::move(overlay));

    g_signal_connect(window_, "destroy", G_CALLBACK(on_destroy_), this);

    gtk_window_present(window_);
}

CallWindow::~CallWindow()
{
    if (window_)
        gtk_window_destroy(window_);
}

// static
void CallWindow::on_destroy_(GtkWidget* /*widget*/, gpointer self)
{
    auto* w = static_cast<CallWindow*>(self);
    w->window_ = nullptr; // already destroyed; prevent double-destroy in dtor
    if (w->on_window_closed)
        w->on_window_closed();
}

// ---------------------------------------------------------------------------

void CallWindow::bring_to_front()
{
    if (window_)
        gtk_window_present(window_);
}

void CallWindow::close_window()
{
    if (window_)
        gtk_window_destroy(window_);
}

void CallWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
        surface_->set_theme(t);
}

void CallWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void CallWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

} // namespace gtk4
#endif // TESSERACT_CALLS_ENABLED
