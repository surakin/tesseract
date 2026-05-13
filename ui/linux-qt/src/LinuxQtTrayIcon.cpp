#include "LinuxQtTrayIcon.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

LinuxQtTrayIcon::LinuxQtTrayIcon(std::function<void()> on_show,
                                 std::function<void()> on_quit,
                                 QObject* parent)
    : QObject(parent),
      on_show_(std::move(on_show)),
      on_quit_(std::move(on_quit))
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        // No StatusNotifierItem host / XEmbed tray on this session.
        // Leave available_ = false; the shell falls back to a real quit
        // on window close.
        return;
    }

    tray_ = std::make_unique<QSystemTrayIcon>(QIcon(":/icons/tesseract.svg"), this);
    tray_->setToolTip(QCoreApplication::applicationName());

    menu_ = std::make_unique<QMenu>();
    QAction* show_action = menu_->addAction(QObject::tr("Show App"));
    QAction* quit_action = menu_->addAction(QObject::tr("Quit"));
    tray_->setContextMenu(menu_.get());

    QObject::connect(show_action, &QAction::triggered, this, [this]{ if (on_show_) on_show_(); });
    QObject::connect(quit_action, &QAction::triggered, this, [this]{ if (on_quit_) on_quit_(); });
    QObject::connect(tray_.get(), &QSystemTrayIcon::activated,
                     this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
            if (on_show_) on_show_();
        }
    });

    tray_->show();
    available_ = tray_->isVisible();
}

LinuxQtTrayIcon::~LinuxQtTrayIcon() {
    if (tray_) tray_->hide();
}

void LinuxQtTrayIcon::set_tooltip(const std::string& text) {
    if (tray_) tray_->setToolTip(QString::fromStdString(text));
}
