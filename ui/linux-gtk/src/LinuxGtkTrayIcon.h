#pragma once
#include <tesseract/tray_icon.h>

#include <functional>
#include <string>

// libayatana-appindicator3 is a GTK3 library; including its headers pulls in
// the full GTK3 type tree. To keep this header safely includable from the
// rest of the GTK4 shell, the underlying handles are type-erased — the real
// types live only in LinuxGtkTrayIcon.cpp, which is compiled in its own
// translation unit (and its own small static library) with the appindicator
// include path applied in isolation.
class LinuxGtkTrayIcon final : public tesseract::ITrayIcon
{
public:
    LinuxGtkTrayIcon(std::function<void()> on_show,
                     std::function<void()> on_quit);
    ~LinuxGtkTrayIcon() override;

    bool is_available() const override
    {
        return available_;
    }
    void set_tooltip(const std::string& text) override;
    void set_unread(bool has_unread, bool has_highlight) override;

private:
    std::function<void()> on_show_;
    std::function<void()> on_quit_;
    void* indicator_ = nullptr; // AppIndicator*
    void* menu_ = nullptr;      // GtkMenu* (GTK3)
    bool available_ = false;
    // Absolute paths to the three pre-rendered tray variants written under
    // $XDG_RUNTIME_DIR/tesseract-<pid>/ at construction.  Empty when variant
    // generation failed (e.g. couldn't locate the base SVG) — set_unread()
    // then no-ops instead of swapping to a missing path.
    std::string normal_icon_path_;
    std::string unread_icon_path_;
    std::string mention_icon_path_;
    // Owned per-process temp directory holding the three PNGs above.
    // Removed (best effort) in the destructor.
    std::string runtime_dir_;
};
