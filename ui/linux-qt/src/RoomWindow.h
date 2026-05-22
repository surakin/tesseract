#pragma once
#include <QWidget>
#include <memory>
#include "app/RoomWindowBase.h"
#include "tk/host_qt.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"

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

protected:
    void resizeEvent(QResizeEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return roomTextArea_.get();
    }

private:
    void show_mention_popup_(tk::Rect cursor_local, int rows);

    MainWindow* parent_shell_;
    tk::qt6::Surface* surface_ = nullptr; // owned by Qt (child widget)
    std::unique_ptr<tk::NativeTextArea> roomTextArea_;

    QWidget* mention_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;
};

} // namespace qt6
