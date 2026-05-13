#pragma once
#include <string>

namespace tesseract {

class ITrayIcon {
public:
    virtual ~ITrayIcon() = default;

    // Returns true if the tray icon was successfully created and is currently
    // visible to the user. Linux without a StatusNotifierItem host or XEmbed
    // tray, or any platform whose underlying API rejected creation, returns
    // false. Callers use this to gate minimize-to-tray behaviour: if false,
    // fall back to a real quit on window close.
    virtual bool is_available() const = 0;

    virtual void set_tooltip(const std::string& text) = 0;
};

} // namespace tesseract
