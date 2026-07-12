#ifdef TESSERACT_CALLS_ENABLED
#include "CallWindow.h"
#include "MainWindow.h"

#include "views/CallOverlayWidget.h"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace qt6
{

CallWindow::CallWindow(MainWindow* parent_shell)
    : QWidget(nullptr, Qt::Window),
      tesseract::CallWindowBase(parent_shell)
{
    setAttribute(Qt::WA_DeleteOnClose, false); // lifetime managed externally
    setWindowTitle(QStringLiteral("Call"));
    resize(640, 480);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    auto overlay = std::make_unique<tesseract::views::CallOverlayWidget>();
    call_overlay_widget_ = overlay.get();
    surface_->set_root(std::move(overlay));

    show();
}

CallWindow::~CallWindow() = default;

// ---------------------------------------------------------------------------

void CallWindow::bring_to_front()
{
    raise();
    activateWindow();
}

void CallWindow::close_window()
{
    close();
}

void CallWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void CallWindow::request_relayout()
{
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
    }
}

void CallWindow::request_repaint()
{
    if (surface_)
        surface_->update();
}

void CallWindow::schedule_delete()
{
    // Defer destruction to after the current Qt event handler returns.
    // Never call delete on a QWidget from inside one of its own event
    // handlers (closeEvent, mouseReleaseEvent, etc.) — Qt's post-event
    // cleanup will dereference the freed object and crash.
    deleteLater();
}

void CallWindow::closeEvent(QCloseEvent* ev)
{
    if (on_window_closed)
        on_window_closed();
    ev->accept();
}

void CallWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
    }
}

} // namespace qt6
#endif // TESSERACT_CALLS_ENABLED
