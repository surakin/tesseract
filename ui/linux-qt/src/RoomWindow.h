#pragma once
#include <QWidget>
#include "app/RoomWindowBase.h"
#include "tk/host_qt.h"

namespace qt6 { class MainWindow; }

namespace qt6 {

// A secondary (pop-out) room window for the Qt6 shell.
class RoomWindow : public QWidget, public tesseract::RoomWindowBase
{
    Q_OBJECT
public:
    RoomWindow(MainWindow* parent_shell, const std::string& room_id);
    ~RoomWindow() override;

    void bring_to_front()                              override;
    void close_window()                                override;
    void request_relayout()                            override;
    void update_window_title_(const std::string& name) override;

protected:
    void resizeEvent(QResizeEvent* ev) override;
    void closeEvent (QCloseEvent*  ev) override;

private:
    MainWindow*       parent_shell_;
    tk::qt6::Surface* surface_ = nullptr;  // owned by Qt (child widget)
};

} // namespace qt6
