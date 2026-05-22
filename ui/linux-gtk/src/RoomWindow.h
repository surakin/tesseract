#pragma once
#include <gtk/gtk.h>
#include "app/RoomWindowBase.h"
#include "tk/host_gtk.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include <memory>

namespace gtk4
{
class MainWindow;
}

namespace gtk4
{

// A secondary (pop-out) room window for the GTK4 shell.
class RoomWindow : public tesseract::RoomWindowBase
{
public:
    RoomWindow(MainWindow* parent_shell, const std::string& room_id);
    ~RoomWindow() override;

    void bring_to_front() override;
    void close_window() override;
    void request_relayout() override;
    void update_window_title_(const std::string& name) override;
    void apply_theme(const tk::Theme& t) override;

protected:
    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return room_text_area_.get();
    }

private:
    void show_mention_popup_(tk::Rect cursor_local, int rows);

    static void on_destroy_(GtkWidget* widget, gpointer self);
    static gboolean on_key_pressed_(GtkEventControllerKey*, guint keyval,
                                    guint, GdkModifierType, gpointer self);

    static void on_copy_action_(GSimpleAction*, GVariant*, gpointer self);

    MainWindow* parent_shell_;
    GtkWindow* window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> surface_;
    GtkWidget* copy_ctx_menu_ = nullptr;
    GSimpleActionGroup* copy_ctx_actions_ = nullptr;

    std::unique_ptr<tk::NativeTextArea> room_text_area_;
    GtkWidget* mention_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;
};

} // namespace gtk4
