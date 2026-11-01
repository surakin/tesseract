#pragma once
#include <gtk/gtk.h>
#include "app/ComposerPopups.h"
#include "app/RoomWindowBase.h"
#include "tk/host_gtk.h"
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/RoomMediaView.h"
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
    void apply_scale_change(float scale) override;
    void repaint_anim_frame() override;

protected:
    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id, const std::string& message) override;

private:
    // Closes any open popup — called from the notify::default-width/-height
    // handlers, GTK4's only per-window resize hook with app context. See
    // gtk4::MainWindow::dismiss_popups_on_resize_ for why.
    void dismiss_popups_on_resize_();

    static void on_destroy_(GtkWidget* widget, gpointer self);
    static gboolean on_key_pressed_(GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                                    gpointer self);
    // Global-scope Ctrl+K shortcut — routes to the main window's quick
    // switcher, bringing the main window forward (the switcher lives there).
    static gboolean on_quick_switch_shortcut_(GtkWidget*, GVariant*, gpointer self);

    static void on_copy_action_(GSimpleAction*, GVariant*, gpointer self);

    MainWindow* parent_shell_;
    GtkWindow* window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> surface_;
    GtkWidget* copy_ctx_menu_ = nullptr;
    GSimpleActionGroup* copy_ctx_actions_ = nullptr;

    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* room_text_area_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr;    // borrowed

    // Composer popups (@mention, /command, :shortcode:, /gif). Declared after
    // surface_ so the popups are destroyed before the Host they came from.
    std::unique_ptr<tesseract::ComposerPopups> popups_;
};

} // namespace gtk4
