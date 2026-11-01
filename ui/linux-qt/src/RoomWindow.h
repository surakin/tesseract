#pragma once
#include <QWidget>
#include <memory>
#include "app/ComposerPopups.h"
#include "app/RoomWindowBase.h"
#include "tk/host_qt.h"
class QMoveEvent;
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/RoomMediaView.h"

namespace qt6
{
class MainWindow;
}

namespace qt6
{

// A secondary (pop-out) room window for the Qt6 shell.
class RoomWindow : public QWidget, public tesseract::RoomWindowBase
{
    Q_OBJECT
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
    void resizeEvent(QResizeEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id, const std::string& message) override;

private:
    MainWindow* parent_shell_;
    tk::qt6::Surface* surface_ = nullptr; // owned by Qt (child widget)
    bool rw_was_maximized_ = false;       // restore target when leaving viewer full-screen
    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* roomTextArea_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr;    // borrowed

    // Composer popups (@mention, /command, :shortcode:, /gif). The popups
    // are destroyed with the window, before Qt tears down the child surface_.
    std::unique_ptr<tesseract::ComposerPopups> popups_;
};

} // namespace qt6
