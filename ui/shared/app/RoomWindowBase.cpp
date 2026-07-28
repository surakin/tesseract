#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"

#include <algorithm>
#include <cmath>

namespace tesseract
{

RoomWindowBase::RoomWindowBase(ShellBase* shell, std::string room_id)
    : shell_(shell), room_id_(std::move(room_id))
{
}

RoomWindowBase::~RoomWindowBase()
{
    if (shell_)
    {
        remove_popout_from_settings_();
        shell_->unregister_room_window_(this);
        shell_->release_room_subscription_(room_id_);
    }
}

void RoomWindowBase::init_pane_(tk::Host* host)
{
    RoomPane::Deps deps;
    deps.shell = shell_;
    deps.host = host;
    deps.repaint = [this] { surface_repaint_(); };
    deps.relayout = [this] { request_relayout(); };
    deps.update_window_title = [this](const std::string& name)
    { update_window_title_(name); };
    // Leaving from a room's own pop-out closes that pop-out, regardless of
    // which room_id is reported (a pop-out only ever shows one room).
    deps.on_left_room = [this](const std::string&) { schedule_self_close_(); };
    // Mirror the main window's search wiring (RoomPane::wire_room_view_'s
    // on_room_search_query), routing through the same ShellBase methods but
    // marking this pop-out's own window as the active search target so
    // ShellBase::in_room_search_navigate_ relayouts the right surface.
    deps.on_search_activated = [this] { shell_->in_room_search_active_win_ = this; };
    pane_ = std::make_unique<RoomPane>(std::move(deps), room_id_);
}

void RoomWindowBase::finish_init_()
{
    shell_->register_room_window_(this);
    shell_->acquire_room_subscription_(room_id_);
    pane_->finish_init();
}

void RoomWindowBase::repaint_anim_frame()
{
    // Default: repaint the room surface so inline animated media advances.
    // Subclasses override to also repaint visible emoji/sticker pickers.
    surface_repaint_();
}

void RoomWindowBase::schedule_self_close_()
{
    if (!shell_)
    {
        return;
    }
    shell_->post_to_ui_(
        [shell = shell_, w = this]
        {
            shell->release_owned_window_(w);
        });
}

Settings::WindowGeometry RoomWindowBase::get_saved_popout_geometry_(
    int default_w, int default_h) const
{
    const auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it == pops.end())
        return {};
    return ShellBase::clamp_to_screens_(it->geometry, default_w, default_h,
                                        shell_->get_screen_work_areas_());
}

Settings::WindowGeometry RoomWindowBase::get_saved_popout_geometry_(
    int default_w, int default_h, int target_dpi) const
{
    const auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it == pops.end())
        return {};
    Settings::WindowGeometry g = it->geometry;
    if (g.valid && g.dpi > 0 && target_dpi > 0 && target_dpi != g.dpi)
    {
        const double s = double(target_dpi) / double(g.dpi);
        g.w = static_cast<int>(std::lround(g.w * s));
        g.h = static_cast<int>(std::lround(g.h * s));
    }
    return ShellBase::clamp_to_screens_(g, default_w, default_h,
                                        shell_->get_screen_work_areas_());
}

void RoomWindowBase::save_popout_geometry_(int x, int y, int w, int h, int dpi)
{
    auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    Settings::WindowGeometry geom;
    geom.x = x; geom.y = y; geom.w = w; geom.h = h;
    geom.dpi = dpi;
    geom.valid = (w > 0 && h > 0);
    if (it != pops.end())
        it->geometry = geom;
    else
    {
        Settings::PopoutEntry e;
        e.room_id  = room_id_;
        e.geometry = geom;
        pops.push_back(std::move(e));
    }
    if (shell_)
        shell_->save_settings_debounced_();
}

void RoomWindowBase::remove_popout_from_settings_()
{
    auto& pops = Settings::instance().popout_windows;
    auto it = std::find_if(pops.begin(), pops.end(),
                           [this](const Settings::PopoutEntry& e)
                           { return e.room_id == room_id_; });
    if (it != pops.end())
    {
        pops.erase(it);
        if (shell_)
            shell_->save_settings_debounced_();
    }
}

} // namespace tesseract
