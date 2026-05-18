#pragma once
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "tk/host.h"
#include "tk/theme.h"
#include <tesseract/types.h>

#include <string>
#include <vector>

namespace tesseract
{

class ShellBase;

// Base class for secondary (pop-out) room windows. Each instance owns the
// per-window state for one room: the borrowed RoomView pointer, compose
// typing state, and the link back to ShellBase for dispatch.
//
// Platform subclasses create a native window + surface, set room_view_, then
// call finish_init_() to register with ShellBase and start the event feed.
// The destructor unregisters and releases the subscription automatically.
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

    // Called by ShellBase on the UI thread when SDK events arrive for this room.
    void on_room_info_updated(const RoomInfo& r);
    void on_timeline_reset(std::vector<views::MessageRowData> rows);
    void on_message_inserted(std::size_t idx, views::MessageRowData row);
    void on_message_updated(std::size_t idx, views::MessageRowData row);
    void on_message_removed(std::size_t idx);
    void on_typing_changed(const std::string& text, bool visible);

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

protected:
    // Call at the end of the subclass constructor, after surface + room_view_
    // are ready. Registers with ShellBase, acquires subscription, and
    // populates the view with the room's current state.
    void finish_init_();

    // Install the shared RoomView providers + compose callbacks on `rv`
    // (which the subclass has just created and parented to its surface).
    // Call after creating room_view_ and before finish_init_(). Uses the
    // shared SDK helpers + shell_ caches, routing the three per-shell
    // primitives through surface_repaint_(), compose_text_area_(), and
    // preview_lookup_(). Surface-bound providers that need the subclass's
    // own surface_ (set_audio_player / set_post_delayed / on_layout_changed)
    // stay in the subclass ctor.
    void wire_room_view_(views::RoomView* rv);

    // The single per-shell repaint primitive (surface->update() /
    // gtk_widget_queue_draw / InvalidateRect / surface->relayout()).
    virtual void surface_repaint_() = 0;

    // Compose text widget to clear after a successful send / prefill on edit
    // / focus on reply, or nullptr if the shell has no native text area and
    // drives compose state through room_view_ instead (Qt6/GTK4 return
    // nullptr; Win32/macOS return their native text area).
    virtual tk::NativeTextArea* compose_text_area_()
    {
        return nullptr;
    }

    // Look up a cached URL-preview card for `url`, or nullptr. The cache
    // (url_preview_data_) lives on the concrete shell, not ShellBase, so
    // each shell forwards this one-liner to its own map.
    virtual const views::UrlPreviewData*
    preview_lookup_(const std::string& url) = 0;

    // Post a deferred call to ShellBase::release_owned_window_(this) on the UI
    // thread. Call from WM_DESTROY (Win32) or the platform destroy callback
    // so the C++ object is deleted safely outside its own message handler.
    void schedule_self_close_();

    // SDK operation helpers — forward to shell_->client_ (accessible via
    // friend class ShellBase). All must be called on the UI thread.
    void send_message_(const std::string& body);
    void send_reply_(const std::string& reply_event_id,
                     const std::string& body);
    void send_edit_(const std::string& event_id, const std::string& new_body);
    void delete_event_(const std::string& event_id);
    void toggle_reaction_(const std::string& event_id, const std::string& key);
    void send_receipt_(const std::string& event_id);
    void send_typing_notice_(bool typing);
    void retry_send_(const std::string& txn_id);
    void abort_send_(const std::string& txn_id);
    void request_pagination_back_();

    // Image cache accessors — friend access to ShellBase protected members
    // so platform subclasses don't need their own friend declarations.
    const tk::Image* shell_avatar_(const std::string& mxc) const;
    const tk::Image* shell_image_(const std::string& mxc) const;
    std::vector<std::uint8_t>
    fetch_source_bytes_(const std::string& source_json);

    ShellBase* shell_;
    std::string room_id_;
    views::RoomView* room_view_ =
        nullptr; // borrowed; owned by surface widget tree
    bool compose_typing_active_ = false;
    bool typing_bar_visible_ = false;
    // First timeline reset = initial fill of this pop-out (gate the display
    // like a room switch); later resets are reconnect/gappy refreshes of the
    // room already shown (refresh in place, no blank).
    bool displayed_once_ = false;
};

} // namespace tesseract
