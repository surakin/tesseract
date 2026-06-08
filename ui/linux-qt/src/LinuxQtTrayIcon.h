#pragma once
#include <tesseract/tray_icon.h>

#include <QObject>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QMenu>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LinuxQtTrayIcon final : public QObject, public tesseract::ITrayIcon
{
    Q_OBJECT
public:
    LinuxQtTrayIcon(std::function<void()> on_show,
                    std::function<void()> on_toggle,
                    std::function<void()> on_quit, QObject* parent = nullptr);
    ~LinuxQtTrayIcon() override;

    bool is_available() const override
    {
        return available_;
    }
    void set_tooltip(const std::string& text) override;
    void set_unread(bool has_unread, bool has_highlight) override;

    // Rebuild the context menu. `window_items` is a list of (label, callback)
    // pairs — one per open main window — prepended before the Quit action.
    // Call from the UI thread only.
    void rebuild_menu(
        std::vector<std::pair<std::string, std::function<void()>>> window_items);

private:
    std::function<void()> on_show_;
    std::function<void()> on_toggle_;
    std::function<void()> on_quit_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    std::unique_ptr<QMenu> menu_;
    bool available_ = false;
    // Rendered once at construction; copied as the source for every overlay
    // composite so the original SVG isn't re-rasterised on every state change.
    QPixmap base_pixmap_;
};
