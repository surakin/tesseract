#pragma once
#include "tk/host.h"

#include <functional>
#include <utility>

namespace tesseract
{

// Schedules `destroy` to run one tick from now via `host`, so a lazily-owned
// view (LoginView, SettingsView) can safely tear itself down from inside a
// callback that originated from its own call stack (a success/close/cancel
// handler fired synchronously from the shared view's own code) without
// freeing memory out from under that still-executing frame. Mirrors
// RoomWindowBase::schedule_self_close_()'s reasoning for popout room
// windows, generalized for a single named member instead of a keyed
// collection.
//
// `host` should be a stable one that outlives the view being torn down
// (e.g. the main app surface's host), not the view's own — that one may be
// mid-destruction by the time a caller might reach for it again.
//
// `is_current` lets the deferred callback detect a same-tick reconstruction
// (the view was recreated before this callback got to run) and skip
// destroying the wrong instance.
inline void schedule_deferred_teardown(tk::Host& host,
                                        std::function<bool()> is_current,
                                        std::function<void()> destroy)
{
    host.post_delayed(0, [is_current = std::move(is_current),
                           destroy = std::move(destroy)]
    {
        if (is_current())
            destroy();
    });
}

} // namespace tesseract
