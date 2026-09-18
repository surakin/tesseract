#include "AuxWindow.h"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QString>
#include <QVBoxLayout>

namespace qt6
{

AuxWindow::AuxWindow(const std::string& title, std::unique_ptr<tk::Widget> root,
                     int width, int height, const tk::Theme& theme)
    : QWidget(nullptr, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose, false); // lifetime managed externally
    setWindowTitle(QString::fromStdString(title));
    resize(width, height);

    surface_ = new tk::qt6::Surface(theme, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);
    surface_->set_root(std::move(root));

    show();
}

AuxWindow::~AuxWindow() = default;

void AuxWindow::bring_to_front()
{
    raise();
    activateWindow();
}

void AuxWindow::close_window()
{
    close();
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
    {
        surface_->relayout();
        surface_->update();
    }
}

void AuxWindow::request_repaint()
{
    if (surface_)
        surface_->update();
}

void AuxWindow::schedule_delete()
{
    // Never delete a QWidget synchronously from inside its own event handler.
    deleteLater();
}

void AuxWindow::closeEvent(QCloseEvent* ev)
{
    if (on_window_closed)
        on_window_closed();
    ev->accept();
}

void AuxWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
    }
}

} // namespace qt6
