#include "AuxWindow.h"
#include "MainWindow.h"

namespace gtk4
{

AuxWindow::AuxWindow(MainWindow* parent_shell, const std::string& title,
                     std::unique_ptr<tk::Widget> root, int width, int height,
                     const tk::Theme& theme)
{
    window_ = GTK_WINDOW(gtk_window_new());

    // Associate with the GtkApplication so this is a proper application window,
    // matching CallWindow / RoomWindow (avoids keyboard-grab issues).
    if (parent_shell && parent_shell->application())
        gtk_window_set_application(window_, parent_shell->application());

    gtk_window_set_title(window_, title.c_str());
    gtk_window_set_default_size(window_, width, height);

    surface_ = std::make_unique<tk::gtk4::Surface>(theme);
    gtk_window_set_child(window_, surface_->widget());
    surface_->set_root(std::move(root));

    g_signal_connect(window_, "destroy", G_CALLBACK(on_destroy_), this);

    gtk_window_present(window_);
}

AuxWindow::~AuxWindow()
{
    if (window_)
        gtk_window_destroy(window_);
}

// static
void AuxWindow::on_destroy_(GtkWidget* /*widget*/, gpointer self)
{
    auto* w = static_cast<AuxWindow*>(self);
    w->window_ = nullptr; // already destroyed; prevent double-destroy in dtor
    if (w->on_window_closed)
        w->on_window_closed();
}

void AuxWindow::bring_to_front()
{
    if (window_)
        gtk_window_present(window_);
}

void AuxWindow::close_window()
{
    if (window_)
        gtk_window_destroy(window_);
}

void AuxWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void AuxWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void AuxWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void AuxWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

} // namespace gtk4
