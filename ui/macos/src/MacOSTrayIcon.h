#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/tray_icon.h>

#include <functional>
#include <memory>
#include <string>

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

private:
    // ARC-managed bridge; held through __strong because this header is
    // compiled both as Objective-C++ and (transitively) plain C++ — under
    // -fobjc-arc the qualifier is required for instance members.
    TesseractTrayBridge* __strong bridge_ = nil;
};
