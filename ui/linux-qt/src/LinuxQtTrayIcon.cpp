#include "LinuxQtTrayIcon.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QColor>
#include <QIcon>
#include <QPainter>

LinuxQtTrayIcon::LinuxQtTrayIcon(std::function<void()> on_show,
                                 std::function<void()> on_toggle,
                                 std::function<void()> on_quit, QObject* parent)
    : QObject(parent), on_show_(std::move(on_show)),
      on_toggle_(std::move(on_toggle)), on_quit_(std::move(on_quit))
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        // No StatusNotifierItem host / XEmbed tray on this session.
        // Leave available_ = false; the shell falls back to a real quit
        // on window close.
        return;
    }

    // Rasterise the base SVG once at a generous size so subsequent overlay
    // composites are pure pixmap operations (no SVG re-rasterisation per state
    // change).  Qt downsamples to the actual tray size on display.
    base_pixmap_ = QIcon(":/icons/tesseract.svg").pixmap(64, 64);
    tray_ = std::make_unique<QSystemTrayIcon>(QIcon(base_pixmap_), this);
    tray_->setToolTip(QCoreApplication::applicationName());

    menu_ = std::make_unique<QMenu>();
    QAction* show_action = menu_->addAction(QObject::tr("Show App"));
    QAction* quit_action = menu_->addAction(QObject::tr("Quit"));
    tray_->setContextMenu(menu_.get());

    QObject::connect(show_action, &QAction::triggered, this,
                     [this]
                     {
                         if (on_show_)
                         {
                             on_show_();
                         }
                     });
    QObject::connect(quit_action, &QAction::triggered, this,
                     [this]
                     {
                         if (on_quit_)
                         {
                             on_quit_();
                         }
                     });
    QObject::connect(tray_.get(), &QSystemTrayIcon::activated, this,
                     [this](QSystemTrayIcon::ActivationReason r)
                     {
                         if (r == QSystemTrayIcon::Trigger ||
                             r == QSystemTrayIcon::DoubleClick)
                         {
                             if (on_toggle_)
                             {
                                 on_toggle_();
                             }
                         }
                     });

    tray_->show();
    available_ = tray_->isVisible();
}

LinuxQtTrayIcon::~LinuxQtTrayIcon()
{
    if (tray_)
    {
        tray_->hide();
    }
}

void LinuxQtTrayIcon::set_tooltip(const std::string& text)
{
    if (tray_)
    {
        tray_->setToolTip(QString::fromStdString(text));
    }
}

void LinuxQtTrayIcon::set_unread(bool has_unread, bool has_highlight)
{
    if (!tray_ || base_pixmap_.isNull())
    {
        return;
    }
    if (!has_unread && !has_highlight)
    {
        tray_->setIcon(QIcon(base_pixmap_));
        return;
    }

    // Composite a coloured dot in the bottom-right corner.  Highlight (server
    // push-rule mention) wins over plain unread, mirroring the per-room badge
    // color logic in RoomListView.
    QPixmap pm = base_pixmap_.copy();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Dot sized at ~38% of the icon edge, anchored to the bottom-right with a
    // small inset so the outline isn't clipped.
    const int side  = std::min(pm.width(), pm.height());
    const int dot   = tesseract::badge_dot_px(side);
    const int inset = tesseract::badge_inset_px(side);
    const int x = pm.width() - dot - inset;
    const int y = pm.height() - dot - inset;

    const unsigned int rgb = has_highlight ? tesseract::kBadgeColorMention
                                           : tesseract::kBadgeColorUnread;
    const QColor fill((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    // White stroke so the dot stays legible on both light and dark trays.
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255), tesseract::badge_inset_px(side)));
    p.drawEllipse(QRect(x, y, dot, dot));
    p.end();

    tray_->setIcon(QIcon(pm));
}
