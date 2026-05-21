#pragma once
#include <string>

namespace tesseract
{

class ITrayIcon
{
public:
    virtual ~ITrayIcon() = default;

    // Returns true if the tray icon was successfully created and is currently
    // visible to the user. Linux without a StatusNotifierItem host or XEmbed
    // tray, or any platform whose underlying API rejected creation, returns
    // false. Callers use this to gate minimize-to-tray behaviour: if false,
    // fall back to a real quit on window close.
    virtual bool is_available() const = 0;

    virtual void set_tooltip(const std::string& text) = 0;

    // Update the unread/mention indicator overlay on the tray icon. Called by
    // ShellBase whenever the aggregate across all signed-in accounts changes.
    // has_highlight implies has_unread (a highlight is a notification) but
    // shells should not assume that — render the dot when either flag is set,
    // preferring the highlight color when both are true. With both false the
    // shell must restore the plain base icon. Default no-op so test fakes and
    // shells without an implementation yet keep working.
    virtual void set_unread(bool /*has_unread*/, bool /*has_highlight*/) {}
};

} // namespace tesseract
