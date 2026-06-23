#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#include <QWidget>
#include "app/CallWindowBase.h"
#include "tk/host_qt.h"

namespace qt6 { class MainWindow; }

namespace qt6
{

// Secondary call pop-out window for the Qt6 shell.
// Hosts a tk::qt6::Surface whose root is a CallOverlayWidget in Popout mode.
class CallWindow : public QWidget, public tesseract::CallWindowBase
{
    Q_OBJECT
public:
    explicit CallWindow(MainWindow* parent_shell);
    ~CallWindow() override;

    void bring_to_front()               override;
    void close_window()                 override;
    void apply_theme(const tk::Theme&)  override;
    void request_relayout()             override;
    void request_repaint()              override;
    void schedule_delete()              override;

protected:
    void closeEvent(QCloseEvent* ev)    override;
    void resizeEvent(QResizeEvent* ev)  override;

private:
    tk::qt6::Surface* surface_ = nullptr; // owned by Qt (child widget)
};

} // namespace qt6
#endif // TESSERACT_CALLS_ENABLED
