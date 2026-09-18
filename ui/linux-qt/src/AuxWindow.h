#pragma once

#include <QWidget>
#include "app/AuxWindowBase.h"
#include "tk/host_qt.h"

#include <memory>
#include <string>

namespace qt6
{

// Generic secondary top-level window for the Qt6 shell: hosts a tk::qt6::Surface
// whose root is whatever widget the shared layer hands in.
class AuxWindow : public QWidget, public tesseract::AuxWindowBase
{
    Q_OBJECT
public:
    AuxWindow(const std::string& title, std::unique_ptr<tk::Widget> root,
              int width, int height, const tk::Theme& theme);
    ~AuxWindow() override;

    void bring_to_front()                override;
    void close_window()                  override;
    void apply_theme(const tk::Theme&)   override;
    void apply_scale_change(float scale) override;
    void request_relayout()              override;
    void request_repaint()               override;
    void schedule_delete()               override;

protected:
    void closeEvent(QCloseEvent* ev)   override;
    void resizeEvent(QResizeEvent* ev) override;

private:
    tk::qt6::Surface* surface_ = nullptr; // owned by Qt (child widget)
};

} // namespace qt6
