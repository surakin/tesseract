#pragma once

#include "app/AuxWindowBase.h"
#include "tk/host_gtk.h"
#include <gtk/gtk.h>
#include <memory>
#include <string>

namespace gtk4 { class MainWindow; }

namespace gtk4
{

// Generic secondary top-level window for the GTK4 shell: hosts a
// tk::gtk4::Surface whose root is whatever widget the shared layer hands in.
class AuxWindow : public tesseract::AuxWindowBase
{
public:
    AuxWindow(MainWindow* parent_shell, const std::string& title,
              std::unique_ptr<tk::Widget> root, int width, int height,
              const tk::Theme& theme);
    ~AuxWindow() override;

    void bring_to_front()                override;
    void close_window()                  override;
    void apply_theme(const tk::Theme&)   override;
    void apply_scale_change(float scale) override;
    void request_relayout()              override;
    void request_repaint()               override;

private:
    static void on_destroy_(GtkWidget* widget, gpointer self);

    GtkWindow*                         window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> surface_;
};

} // namespace gtk4
