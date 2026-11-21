#pragma once

#include "views/CameraWidget.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace tesseract::views
{

// Owns the lifecycle of one selfie CameraWidget inside a parent widget (the
// main window's overlay stack, or a pop-out room window's root). One
// implementation so the /selfie command works the same in every window.
class CameraOverlayHost
{
public:
    using CapturedFn =
        std::function<void(std::vector<std::uint8_t> bgra, std::uint32_t w, std::uint32_t h)>;

    // `parent` receives the CameraWidget as its newest (topmost) child.
    // `layout_changed` is called after the overlay is added or removed.
    CameraOverlayHost(tk::Widget* parent, std::function<void()> layout_changed);

    // Receives the raw BGRA8888 frame after the countdown. Set before open().
    CapturedFn on_captured;
    // open() is a no-op while this returns true.
    std::function<bool()> is_call_active;

    // Opens the overlay; a no-op while one is already open.
    void open();
    // Removes the overlay (also called when it dismisses itself).
    void close();
    bool is_open() const
    {
        return widget_ != nullptr;
    }
    CameraWidget* widget() const
    {
        return widget_;
    }

private:
    tk::Widget* parent_;
    std::function<void()> layout_changed_;
    CameraWidget* widget_ = nullptr;
};

} // namespace tesseract::views
