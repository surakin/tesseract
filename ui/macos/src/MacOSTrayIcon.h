#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/tray_icon.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declared Obj-C glue that owns the NSStatusItem + menu and routes
// menu-item actions back to the C++ std::function callbacks.
@class TesseractTrayBridge;

class MacOSTrayIcon final : public tesseract::ITrayIcon
{
public:
    MacOSTrayIcon(std::function<void()> on_show, std::function<void()> on_quit);
    ~MacOSTrayIcon() override;

    bool is_available() const override
    {
        return bridge_ != nil;
    }
    void set_tooltip(const std::string& text) override;
    void set_unread(bool has_unread, bool has_highlight) override;

    // Rebuild the right-click context menu. `window_items` is a list of
    // (label, callback) pairs — one per open main window — placed before the
    // Quit action. Call from the main thread only.
    void rebuild_menu(
        std::vector<std::pair<std::string, std::function<void()>>> window_items);

private:
    // ARC-managed bridge; held through __strong because this header is
    // compiled both as Objective-C++ and (transitively) plain C++ — under
    // -fobjc-arc the qualifier is required for instance members.
    TesseractTrayBridge* __strong bridge_ = nil;
};
