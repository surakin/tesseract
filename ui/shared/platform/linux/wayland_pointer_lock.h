#pragma once

// Pointer lock on Wayland: zwp_pointer_constraints_v1 keeps the pointer where it is and
// zwp_relative_pointer_v1 reports how far it moved. Neither Qt nor GTK offers this, and a Wayland
// client cannot warp the cursor, so it is the only way to look around with the mouse. Toolkit
// neutral: the shell hands over the raw wayland objects. Built only with
// TESSERACT_WAYLAND_POINTER_LOCK (wayland-client found).

#include <functional>

struct wl_display;
struct wl_surface;
struct wl_pointer;
struct zwp_locked_pointer_v1;
struct zwp_relative_pointer_v1;

namespace tesseract::platform
{

class WaylandPointerLock
{
public:
    // dx and dy are in the surface's logical pixels. `lost` is called when the compositor ends
    // the lock (focus moved to another surface).
    using Motion = std::function<void(float dx, float dy)>;
    using Lost = std::function<void()>;

    WaylandPointerLock() = default;
    ~WaylandPointerLock();
    WaylandPointerLock(const WaylandPointerLock&) = delete;
    WaylandPointerLock& operator=(const WaylandPointerLock&) = delete;

    // Whether the compositor behind `display` offers both protocols.
    static bool available(wl_display* display);

    // Locks `pointer` to `surface`, which must hold the pointer focus (it does while the user
    // clicks on it). False when unavailable. Replaces an earlier lock.
    bool lock(wl_display* display, wl_surface* surface, wl_pointer* pointer, Motion motion,
              Lost lost);
    void unlock();
    bool locked() const
    {
        return constraint_ != nullptr;
    }

    // For the protocol listeners only.
    void deliver_motion(float dx, float dy);
    void deliver_unlocked();

private:
    wl_display* display_ = nullptr;
    Motion motion_;
    Lost lost_;
    zwp_locked_pointer_v1* constraint_ = nullptr;
    zwp_relative_pointer_v1* relative_ = nullptr;
};

} // namespace tesseract::platform
