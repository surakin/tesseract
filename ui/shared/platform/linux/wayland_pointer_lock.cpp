#include "platform/linux/wayland_pointer_lock.h"

#include <wayland-client.h>

#include "platform/linux/wayland/pointer-constraints-unstable-v1-client-protocol.h"
#include "platform/linux/wayland/relative-pointer-unstable-v1-client-protocol.h"

#include <cstdint>
#include <cstring>
#include <utility>

namespace tesseract::platform
{

namespace
{

zwp_pointer_constraints_v1* s_constraints = nullptr;
zwp_relative_pointer_manager_v1* s_relative_manager = nullptr;
wl_display* s_bound_display = nullptr;

void on_global(void*, wl_registry* registry, std::uint32_t name, const char* interface,
               std::uint32_t)
{
    if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0)
    {
        s_constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    }
    else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0)
    {
        s_relative_manager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}

void on_global_remove(void*, wl_registry*, std::uint32_t)
{
}

const wl_registry_listener kRegistryListener = {on_global, on_global_remove};

// Binds the two globals once per display (a process has one).
void bind_globals(wl_display* display)
{
    if (s_bound_display == display)
    {
        return;
    }
    s_bound_display = display;
    s_constraints = nullptr;
    s_relative_manager = nullptr;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, nullptr);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
}

void relative_motion(void* data, zwp_relative_pointer_v1*, std::uint32_t, std::uint32_t,
                     wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
{
    static_cast<WaylandPointerLock*>(data)->deliver_motion(
        static_cast<float>(wl_fixed_to_double(dx)), static_cast<float>(wl_fixed_to_double(dy)));
}

void locked(void*, zwp_locked_pointer_v1*)
{
}

// One-shot lock: the compositor ended it (focus moved away, or it refused the lock).
void unlocked(void* data, zwp_locked_pointer_v1*)
{
    static_cast<WaylandPointerLock*>(data)->deliver_unlocked();
}

const zwp_relative_pointer_v1_listener kRelativeListener = {relative_motion};
const zwp_locked_pointer_v1_listener kLockedListener = {locked, unlocked};

} // namespace

WaylandPointerLock::~WaylandPointerLock()
{
    unlock();
}

bool WaylandPointerLock::available(wl_display* display)
{
    if (display == nullptr)
    {
        return false;
    }
    bind_globals(display);
    return s_constraints != nullptr && s_relative_manager != nullptr;
}

bool WaylandPointerLock::lock(wl_display* display, wl_surface* surface, wl_pointer* pointer,
                              Motion motion, Lost lost)
{
    unlock();
    if (!available(display) || surface == nullptr || pointer == nullptr)
    {
        return false;
    }
    display_ = display;
    motion_ = std::move(motion);
    lost_ = std::move(lost);
    constraint_ = zwp_pointer_constraints_v1_lock_pointer(
        s_constraints, surface, pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    relative_ = zwp_relative_pointer_manager_v1_get_relative_pointer(s_relative_manager, pointer);
    zwp_locked_pointer_v1_add_listener(constraint_, &kLockedListener, this);
    zwp_relative_pointer_v1_add_listener(relative_, &kRelativeListener, this);
    wl_display_flush(display_);
    return true;
}

void WaylandPointerLock::unlock()
{
    if (constraint_)
    {
        zwp_locked_pointer_v1_destroy(constraint_);
        constraint_ = nullptr;
    }
    if (relative_)
    {
        zwp_relative_pointer_v1_destroy(relative_);
        relative_ = nullptr;
    }
    motion_ = nullptr;
    lost_ = nullptr;
    if (display_)
    {
        wl_display_flush(display_);
    }
}

void WaylandPointerLock::deliver_motion(float dx, float dy)
{
    if (motion_)
    {
        motion_(dx, dy);
    }
}

void WaylandPointerLock::deliver_unlocked()
{
    Lost lost = lost_;
    unlock();
    if (lost)
    {
        lost();
    }
}

} // namespace tesseract::platform
