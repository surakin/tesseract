#pragma once
#include "app/RoomPane.h"
#include "views/RoomView.h"
#include "tk/host.h"
#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{
class ImageViewerOverlay;
class VideoViewerOverlay;
} // namespace tesseract::views

namespace tesseract
{

class ShellBase;

// Base class for secondary (pop-out) room windows. Each instance owns the
// per-window chrome for one room: the native window/surface, geometry
// persistence, and the ShellBase registry/subscription lifecycle. The actual
// room *display* — RoomView wiring, SDK send/edit/react/pin operations, the
// thread panel, and the composer popup-hooks/geometry helpers — is owned by
// a RoomPane member (see RoomPane.h), shared with the main window's own
// currently-displayed-room pane.
//
// Platform subclasses create a native window + surface, set room_view_ (and
// optionally img_viewer_/vid_viewer_), call init_pane_() once their Host
// exists, then pane_->attach(...) + finish_init_() to register with
// ShellBase and start the event feed. The destructor unregisters and
// releases the subscription automatically.
class RoomWindowBase
{
public:
    RoomWindowBase(ShellBase* shell, std::string room_id);
    virtual ~RoomWindowBase();

    const std::string& room_id() const
    {
        return room_id_;
    }
    views::RoomView* room_view() const
    {
        return room_view_;
    }

    // Called by ShellBase on the UI thread when SDK events arrive for this
    // room. Forwarded to pane_ so ShellBase::dispatch_to_secondary_windows_'s
    // existing call sites need no changes.
    void on_room_info_updated(const RoomInfo& r) { pane_->on_room_info_updated(r); }
    void on_timeline_reset(std::vector<views::MessageRowData> rows)
    {
        pane_->on_timeline_reset(std::move(rows));
    }
    void on_message_inserted(std::size_t idx, views::MessageRowData row)
    {
        pane_->on_message_inserted(idx, std::move(row));
    }
    void on_message_updated(std::size_t idx, views::MessageRowData row)
    {
        pane_->on_message_updated(idx, std::move(row));
    }
    void on_message_removed(std::size_t idx) { pane_->on_message_removed(idx); }
    void on_typing_changed(const std::string& text, bool visible)
    {
        pane_->on_typing_changed(text, visible);
    }

    // Thread view delivery — called by ShellBase when SDK events arrive for
    // the thread this pop-out has open (popout_thread_root() matches).
    const std::string& popout_thread_root() const { return pane_->thread_root(); }
    void apply_thread_reset_(std::vector<views::MessageRowData> rows)
    {
        pane_->apply_thread_reset_(std::move(rows));
    }
    void apply_thread_prepend_(std::vector<views::MessageRowData> rows)
    {
        pane_->apply_thread_prepend_(std::move(rows));
    }
    void apply_thread_append_(std::vector<views::MessageRowData> rows)
    {
        pane_->apply_thread_append_(std::move(rows));
    }
    void apply_thread_insert_(std::size_t index, views::MessageRowData row)
    {
        pane_->apply_thread_insert_(index, std::move(row));
    }
    void apply_thread_update_(std::size_t index, views::MessageRowData row)
    {
        pane_->apply_thread_update_(index, std::move(row));
    }
    void apply_thread_remove_(std::size_t index) { pane_->apply_thread_remove_(index); }

    // Fan-in for async GIF search results. ShellBase forwards every result to
    // every open pop-out (the GIF search request_id is process-global, so only
    // the controller that issued it matches; the rest drop it). Default no-op
    // for subclasses without a GIF strip; the Qt/GTK/Win32 pop-outs override to
    // forward into their GifController. Not part of RoomPane — the GIF strip
    // controller stays a per-platform member.
    virtual void on_gif_results(std::uint64_t /*request_id*/,
                                std::vector<GifResult> /*results*/)
    {
    }
    virtual void on_gif_search_failed(std::uint64_t /*request_id*/,
                                      const std::string& /*message*/)
    {
    }

    // Fan-in for async message-forward completions, mirroring on_gif_results
    // above: ShellBase::forward_event's request_id is process-global, so
    // ShellBase checks its own main-window pending_forwards_ first, then
    // calls this on every open pop-out until one recognizes the id. Returns
    // true if this window issued request_id (and updated its own picker
    // accordingly), so the caller can stop looking.
    bool handle_forward_done_(std::uint64_t request_id)
    {
        return pane_->handle_forward_done_(request_id);
    }
    bool handle_forward_failed_(std::uint64_t request_id,
                                const std::string& message)
    {
        return pane_->handle_forward_failed_(request_id, message);
    }

    // Called by ShellBase::tick_anim_ on every animation frame so this window's
    // animated images (inline media, and any open pickers) advance even when
    // the pointer is still. The base repaints the room surface; subclasses
    // override to also repaint visible emoji/sticker pickers.
    virtual void repaint_anim_frame();

    // Returns true if this secondary window is currently on-screen (not
    // minimized, not hidden). Default: true — conservative, so platforms
    // that do not override never accidentally starve the animation timer.
    virtual bool is_visible() const { return true; }

    // Platform overrides.
    virtual void bring_to_front() = 0;
    virtual void close_window() = 0;
    virtual void request_relayout() = 0;
    virtual void update_window_title_(const std::string& /*name*/)
    {
    }
    // Re-theme this pop-out window's surface (and any native chrome).
    // Called by the shell's apply_theme_ui_() so secondary windows track
    // the theme setting just like the main window.
    virtual void apply_theme(const tk::Theme& t) = 0;
    // Push a device-pixel scale change through this pop-out window's
    // surface. Unlike apply_theme (a single global setting the shell
    // broadcasts to every window from one place), scale is per-monitor:
    // this is an independent top-level window the user can drag to a
    // different-DPI display on its own, so each platform's implementation
    // observes and reacts to its own scale-change notification directly
    // (Win32: its own WM_DPICHANGED; macOS: its own
    // NSWindowDidChangeBackingPropertiesNotification observer; Qt6/GTK4:
    // its own Surface self-observes internally) rather than being told by
    // the shell. Keeps this window's native-control image captures
    // (tk::NativeTextField/NativeTextArea) from staying stale/blurry
    // independent of the main window's own scale.
    virtual void apply_scale_change(float scale) = 0;

protected:
    // Construct pane_ once the subclass's Host exists (i.e. once its Surface
    // has been created). Call this where the subclass used to call
    // wire_room_view_(room_view_), immediately followed by pane_->attach(...)
    // and, at the very end of the subclass constructor, finish_init_().
    void init_pane_(tk::Host* host);

    // Call at the end of the subclass constructor, after init_pane_() and
    // pane_->attach(...) have run. Registers with ShellBase, acquires the
    // room subscription, and delegates the per-room display seed to pane_.
    void finish_init_();

    // The single per-shell repaint primitive (surface->update() /
    // gtk_widget_queue_draw / InvalidateRect). Wired into pane_'s Deps by
    // init_pane_(), and used directly by repaint_anim_frame()'s default body.
    virtual void surface_repaint_() = 0;

    // Post a deferred call to ShellBase::release_owned_window_(this) on the UI
    // thread. Call from WM_DESTROY (Win32) or the platform destroy callback
    // so the C++ object is deleted safely outside its own message handler.
    void schedule_self_close_();

    // Look up the saved geometry for this room's popout from Settings,
    // validated against available screens. Returns {valid=false} when there
    // is no saved entry — callers should fall back to their platform default.
    // Intended for use in the subclass constructor, after shell_ is set.
    Settings::WindowGeometry get_saved_popout_geometry_(int default_w,
                                                        int default_h) const;

    // Write the current native window position/size into Settings and
    // schedule a debounced save. Call from resize/move callbacks.
    // Pass dpi=GetDpiForWindow(hwnd) on Win32 so that geometry can be
    // rescaled correctly when restored on a monitor at a different DPI.
    void save_popout_geometry_(int x, int y, int w, int h, int dpi = 0);

    // Like get_saved_popout_geometry_ but rescales w/h from the saved DPI
    // to target_dpi before clamping. Pass target_dpi=0 to skip rescaling
    // (equivalent to the no-target_dpi overload). Win32 callers should pass
    // GetDpiForMonitor(...) for the monitor at the saved position.
    Settings::WindowGeometry get_saved_popout_geometry_(int default_w,
                                                        int default_h,
                                                        int target_dpi) const;

    // Remove this room's entry from Settings::popout_windows and save.
    // Called automatically by the destructor; also callable on explicit close.
    void remove_popout_from_settings_();

    ShellBase* shell_;
    std::string room_id_;
    views::RoomView* room_view_ =
        nullptr; // borrowed; owned by surface widget tree. Also handed to
                 // pane_ via attach() — kept here too since platform ctors
                 // read this member directly throughout their own wiring.
    // Media overlays — set by the subclass ctor before init_pane_()/attach().
    // When non-null, image/video click callbacks are wired (by pane_) to
    // open them.
    views::ImageViewerOverlay* img_viewer_ = nullptr; // borrowed
    views::VideoViewerOverlay* vid_viewer_ = nullptr; // borrowed
    // Debounce flag for the native compose text area's typing-notice timer.
    // Only Win32/macOS wire this directly (Qt6/GTK4 debounce differently);
    // not part of pane_'s shared wiring, so it stays here.
    bool compose_typing_active_ = false;

    // Owns this window's per-room display state and wiring. Constructed by
    // init_pane_() once the subclass's Host exists (RoomWindowBase's own
    // constructor runs before any subclass Surface does, so this can't be a
    // plain value member built in the initializer list).
    std::unique_ptr<RoomPane> pane_;
};

} // namespace tesseract
