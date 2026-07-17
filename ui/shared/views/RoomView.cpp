#include "RoomView.h"

#include "icons.h"
#include "tesseract/settings.h"
#include "tk/i18n.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace tesseract::views
{

namespace
{
// Count of Image/Video rows currently synced into the timeline — i.e. only
// what's already loaded locally, not a server-side total. Feeds
// RoomInfoPanel's "Media (N)" row; grows as more history is paginated in
// (including via the room-media gallery's own backward pagination).
int count_synced_media_(const std::vector<MessageRowData>& rows)
{
    int n = 0;
    for (const auto& r : rows)
    {
        if (r.kind == MessageRowData::Kind::Image ||
            r.kind == MessageRowData::Kind::Video)
        {
            ++n;
        }
    }
    return n;
}
} // namespace

// Transparent, paints-nothing widget that sits on top of message_list_ while
// the thread panel is open. Claims pointer-down (so the message list never
// sees a click) and fires on_click on release; returns true from pointer
// move so dispatch_pointer_move stops here, preventing the message list
// from updating its hover state.
class RoomView::MessageBlocker : public tk::Widget
{
public:
    std::function<void()> on_click;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return constraints;
    }
    bool on_pointer_down(tk::Point /*local*/) override
    {
        pressed_ = true;
        return true;
    }
    void on_pointer_up(tk::Point /*local*/, bool inside_self) override
    {
        const bool fire = pressed_ && inside_self;
        pressed_ = false;
        if (fire && on_click) on_click();
    }
    bool on_pointer_move(tk::Point /*local*/) override
    {
        return false;
    }
    bool on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/) override
    {
        // Swallow wheel input on the dimmed main column. Without this,
        // wheel over the floating thread overlay falls through to
        // message_list_ whenever the thread view's MessageListView
        // declines (e.g. its room-switch gate is still up), scrolling
        // the main timeline behind the panel.
        return true;
    }
    void paint(tk::PaintCtx&) override
    {
        // Intentionally empty — the dim overlay is painted by MessageListView
        // itself; this widget is purely an input shield.
    }

private:
    bool pressed_ = false;
};

RoomView::RoomView()
{
    auto brand = std::make_unique<BrandView>();
    brand_view_ = add_child(std::move(brand));

    auto header = std::make_unique<RoomHeader>();
    header_ = add_child(std::move(header));

    auto msg = std::make_unique<MessageListView>();
    message_list_ = add_child(std::move(msg));

    // Added immediately after message_list_ so it dispatches BEFORE the
    // message list (children walk reverse in dispatch_pointer_*), capturing
    // clicks and hover while the thread panel is open. Hidden by default.
    auto blocker = std::make_unique<MessageBlocker>();
    message_blocker_ = add_child(std::move(blocker));
    message_blocker_->set_visible(false);
    message_blocker_->on_click = [this] {
        // The shell's CloseThread trigger only fires when an individual
        // thread is open — it's a no-op for List mode. Pick the callback
        // that actually closes the panel in each state.
        if (thread_panel_state_ == ThreadPanelState::Open)
        {
            if (on_thread_close_requested) on_thread_close_requested();
        }
        else if (thread_panel_state_ == ThreadPanelState::List)
        {
            if (on_threads_button_clicked) on_threads_button_clicked();
        }
    };

    auto compose = tk::create_widget<ComposeBar>(this);
    compose_bar_ = add_child(std::move(compose));
    compose_bar_->set_enabled(false);

    auto room_info = tk::create_widget<RoomInfoPanel>(this);
    room_info_panel_ = add_child(std::move(room_info));

    auto room_settings = tk::create_widget<RoomSettingsView>(this);
    room_settings_view_ = add_child(std::move(room_settings));

    auto user_profile = std::make_unique<UserProfilePanel>();
    user_profile_panel_ = add_child(std::move(user_profile));

    // Overflow popup — added last so it paints and dispatches above all other
    // children. Remains zero-area (invisible to hit-testing) while closed.
    auto overflow = std::make_unique<PopupMenu>();
    overflow_menu_ = add_child(std::move(overflow));
    overflow_menu_->on_dismissed = [this]
    {
        if (message_list_)
            message_list_->set_hover_locked(false);
        if (thread_view_ && thread_view_->message_list())
            thread_view_->message_list()->set_hover_locked(false);
        overflow_menu_->close();
    };
    overflow_menu_->on_layout_changed = [this]
    {
        if (on_layout_changed)
            on_layout_changed();
    };

    // Call-type popup — anchored to the call button; opened by the
    // on_call_requested handler below when the button is pressed.
    {
        auto cp = std::make_unique<PopupMenu>();
        call_popup_ = add_child(std::move(cp));
        call_popup_->on_dismissed = [this] { call_popup_->close(); };
        call_popup_->on_layout_changed = [this]
        {
            if (on_layout_changed)
                on_layout_changed();
        };
    }

    // In-room search bar — docked strip under the header. Hidden until
    // open_room_search() is called.
    auto search_bar = tk::create_widget<RoomSearchBar>(this);
    room_search_bar_ = add_child(std::move(search_bar));
    room_search_bar_->set_visible(false);
    room_search_bar_->on_query_changed = [this](const std::string& q)
    {
        if (on_room_search_query) on_room_search_query(q);
    };
    room_search_bar_->on_navigate = [this](int delta)
    {
        if (on_room_search_navigate) on_room_search_navigate(delta);
    };
    room_search_bar_->on_paginate_toggled = [this](bool enabled)
    {
        if (on_room_search_paginate_toggled) on_room_search_paginate_toggled(enabled);
    };
    room_search_bar_->on_close = [this] { close_room_search(); };

    if (header_)
        header_->on_search_requested = [this] { open_room_search(); };

    auto banner = std::make_unique<IncomingCallBanner>();
    call_banner_ = add_child(std::move(banner));

    if (header_)
        header_->on_call_requested = [this](tk::Rect btn_rect)
        {
            if (!call_popup_) return;
            const std::string rid = current_room_info_.id;
            PopupMenu::Item audio;
            audio.svg_icon    = kPhoneSvg;
            audio.label       = "Audio call";
            audio.on_selected = [this, rid]
            {
                if (on_start_call) on_start_call(rid, "call#default", true);
            };
            PopupMenu::Item video;
            video.svg_icon    = kVideoSvg;
            video.label       = "Video call";
            video.on_selected = [this, rid]
            {
                if (on_start_call) on_start_call(rid, "call#default", false);
            };
            call_popup_->open({audio, video}, btn_rect);
        };

    // Full-bleed media gallery — added last so it paints and dispatches
    // above every other child in this room, including the other overlay
    // panels above. Hidden until opened via RoomInfoPanel's "Media (N)" row.
    auto rmv = std::make_unique<RoomMediaView>();
    room_media_view_ = add_child(std::move(rmv));
    room_media_view_->set_visible(false);

    wire_internal_callbacks();
}

void RoomView::wire_message_list_callbacks_(MessageListView* ml)
{
    if (!ml) return;

    // Reply flow: hover ↩ → compose enters reply mode + focus.
    ml->on_reply_requested = [this](const std::string& event_id,
                                    const std::string& sender_name,
                                    const std::string& body_preview)
    {
        compose_bar_->set_reply_to(event_id, sender_name, body_preview);
        compose_bar_->focus();
    };

    // Edit flow: hover ✏ → compose enters edit mode, shell prefills textarea.
    ml->on_edit_requested =
        [this](const std::string& event_id, const std::string& current_body)
    {
        compose_bar_->set_editing(event_id);
        compose_bar_->set_current_text(current_body);
        if (on_edit_prefill) on_edit_prefill(current_body);
    };

    // Clipboard write — forwarded to the shell via on_set_clipboard.
    ml->on_set_clipboard = [this](std::string_view text)
    {
        if (on_set_clipboard) on_set_clipboard(text);
    };

    ml->on_more_requested =
        [this, ml](const std::string& event_id, tk::Rect anchor,
                   bool can_delete, bool can_pin, bool is_pinned,
                   bool can_forward)
    {
        // anchor is in world coordinates — PopupMenu::open() takes world coords.
        std::vector<PopupMenu::Item> items;
        if (can_forward)
        {
            items.push_back({"",
                             kForwardSvg,
                             tk::tr("Forward message"), /*destructive=*/false,
                             [this, event_id]
                             {
                                 if (on_forward_requested)
                                     on_forward_requested(event_id);
                             }});
        }
        if (can_delete)
        {
            items.push_back({"", // SVG icon below
                             kRedactSvg,
                             tk::tr("Delete message"), /*destructive=*/true,
                             [this, event_id]
                             {
                                 if (on_delete_requested)
                                     on_delete_requested(event_id);
                             }});
        }
        if (can_pin)
        {
            items.push_back({"", // SVG icon below
                             kPinSvg,
                             is_pinned ? tk::tr("Unpin message")
                                       : tk::tr("Pin message"),
                             /*destructive=*/false,
                             [this, event_id, is_pinned]
                             {
                                 if (is_pinned)
                                 {
                                     if (on_unpin_requested)
                                         on_unpin_requested(event_id);
                                 }
                                 else
                                 {
                                     if (on_pin_requested)
                                         on_pin_requested(event_id);
                                 }
                             }});
        }
        if (tesseract::Settings::instance().developer_mode)
        {
            items.push_back({"", // SVG icon below
                             kCopySvg,
                             tk::tr("Copy event source"), /*destructive=*/false,
                             [this, event_id]
                             {
                                 if (on_copy_event_source_requested)
                                     on_copy_event_source_requested(event_id);
                             }});
        }
        if (!items.empty())
        {
            ml->set_hover_locked(true);
            overflow_menu_->open(std::move(items), anchor);
        }
    };
    ml->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key,
               const std::string& source_mxc)
    {
        if (on_reaction_toggled) on_reaction_toggled(event_id, key, source_mxc);
    };
    ml->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor)
    {
        if (on_add_reaction_requested) on_add_reaction_requested(event_id, anchor);
    };
    ml->on_link_clicked = [this](const std::string& url)
    {
        if (on_link_clicked) on_link_clicked(url);
        // Clicking anywhere on the canvas steals OS focus from the native
        // text area overlay. Restore it after the browser opens.
        compose_bar_->focus();
    };
    ml->on_link_hovered = [this](const std::string& url)
    {
        if (on_link_hovered) on_link_hovered(url);
    };
    ml->on_receipt_needed = [this](const std::string& event_id)
    {
        if (on_receipt_needed) on_receipt_needed(event_id);
    };
    ml->on_visible_range_changed =
        [this](const std::vector<std::string>& keys)
    {
        if (on_visible_range_changed) on_visible_range_changed(keys);
    };
    ml->on_visible_avatars_changed =
        [this](const std::vector<std::string>& urls)
    {
        if (on_visible_avatars_changed) on_visible_avatars_changed(urls);
    };
    ml->on_image_clicked = [this](const MessageListView::ImageHit& hit)
    {
        if (on_image_clicked) on_image_clicked(hit);
    };
    ml->on_video_clicked = [this](const MessageListView::VideoHit& hit)
    {
        if (on_video_clicked) on_video_clicked(hit);
    };
    ml->on_file_clicked = [this](const MessageListView::FileHit& hit)
    {
        if (on_file_clicked) on_file_clicked(hit);
    };
    ml->on_scroll_to_original =
        [this](const std::string& original_event_id)
    {
        if (on_scroll_to_original) on_scroll_to_original(original_event_id);
    };
    ml->on_sender_clicked =
        [this](std::string uid, std::string name, std::string av)
    {
        show_user_profile(std::move(uid), std::move(name), std::move(av));
    };
    // Inline mention pill → resolve the clicked user against the current
    // room's members and open the profile panel.
    ml->on_mention_clicked = [this](const std::string& uid)
    {
        std::string name, avatar;
        for (const auto& m : room_members_)
        {
            if (m.user_id == uid)
            {
                name   = m.display_name;
                avatar = m.avatar_url;
                break;
            }
        }
        show_user_profile(uid, name, avatar);
    };
}

void RoomView::wire_internal_callbacks()
{
    // Wire every MessageListView callback that's identical between the main
    // timeline and the thread panel. Timeline-only ones (pagination, thread
    // preview chips) stay below — see comments at each site.
    wire_message_list_callbacks_(message_list_);

    // Forward compose callbacks that reach the shell.
    compose_bar_->on_size_changed = [this]
    {
        if (on_layout_changed)
        {
            on_layout_changed();
        }
    };
    compose_bar_->on_request_anim_repaint_ = [this](int delay_ms)
    {
        if (post_delayed_ && repaint_requester_ && !anim_repaint_pending_)
        {
            anim_repaint_pending_ = true;
            post_delayed_(delay_ms,
                          [this]
                          {
                              anim_repaint_pending_ = false;
                              repaint_requester_();
                          });
        }
    };
    compose_bar_->on_send = [this](const std::string& body)
    {
        if (thread_panel_state_ == ThreadPanelState::Open)
        {
            if (on_thread_send) on_thread_send(body, std::string{});
        }
        else if (on_send)
        {
            on_send(body);
        }
    };
    compose_bar_->on_send_reply =
        [this](const std::string& reply_id, const std::string& body)
    {
        if (thread_panel_state_ == ThreadPanelState::Open)
        {
            if (on_thread_send_reply)
                on_thread_send_reply(reply_id, body, std::string{});
            return;
        }
        if (on_send_reply)
        {
            on_send_reply(reply_id, body);
        }
    };
    compose_bar_->on_send_edit =
        [this](const std::string& event_id, const std::string& new_body)
    {
        if (on_send_edit)
        {
            on_send_edit(event_id, new_body);
        }
    };
    compose_bar_->on_send_image =
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename, std::string caption, std::uint32_t w,
               std::uint32_t h, bool is_animated, std::string reply_id)
    {
        if (on_send_image)
        {
            on_send_image(std::move(bytes), std::move(mime),
                          std::move(filename), std::move(caption),
                          static_cast<int>(w), static_cast<int>(h), is_animated,
                          std::move(reply_id));
        }
    };
    compose_bar_->on_send_file =
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename, std::string caption, std::string reply_id)
    {
        if (on_send_file)
        {
            on_send_file(std::move(bytes), std::move(mime), std::move(filename),
                         std::move(caption), std::move(reply_id));
        }
    };
    compose_bar_->on_send_video =
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename, std::string caption,
               std::uint32_t w, std::uint32_t h,
               std::vector<std::uint8_t> thumb_bytes,
               std::uint32_t tw, std::uint32_t th,
               std::uint64_t duration_ms, std::string reply_id)
    {
        if (on_send_video)
        {
            on_send_video(std::move(bytes), std::move(mime),
                          std::move(filename), std::move(caption),
                          static_cast<int>(w), static_cast<int>(h),
                          std::move(thumb_bytes),
                          static_cast<int>(tw), static_cast<int>(th),
                          duration_ms, std::move(reply_id));
        }
    };
    compose_bar_->on_send_audio =
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename, std::string caption,
               std::uint64_t duration_ms, std::string reply_id)
    {
        if (on_send_audio)
        {
            on_send_audio(std::move(bytes), std::move(mime),
                          std::move(filename), std::move(caption),
                          duration_ms, std::move(reply_id));
        }
    };
    compose_bar_->on_mic_clicked = [this]
    {
        if (on_mic_clicked)
            on_mic_clicked();
    };
    compose_bar_->on_cancel_voice = [this]
    {
        if (on_cancel_voice)
            on_cancel_voice();
    };
    compose_bar_->on_edit_cancelled = [this]
    {
        if (on_edit_cancelled)
        {
            on_edit_cancelled();
        }
    };
    compose_bar_->on_emoji = [this](tk::Rect r)
    {
        if (on_emoji)
        {
            on_emoji(r);
        }
    };
    compose_bar_->on_sticker = [this](tk::Rect r)
    {
        if (on_sticker)
        {
            on_sticker(r);
        }
    };

    header_->on_link_clicked = [this](const std::string& url)
    {
        if (on_link_clicked)
        {
            on_link_clicked(url);
        }
        compose_bar_->focus();
    };
    header_->on_link_hovered = [this](const std::string& url)
    {
        if (on_link_hovered) on_link_hovered(url);
    };
    message_list_->on_near_top = [this]
    {
        if (on_near_top)
        {
            on_near_top();
        }
    };
    message_list_->on_near_bottom = [this]
    {
        if (on_near_bottom)
        {
            on_near_bottom();
        }
    };
    message_list_->on_return_to_live = [this]
    {
        if (on_return_to_live)
        {
            on_return_to_live();
        }
    };
    header_->on_date_jump = [this](std::uint64_t ts_ms)
    {
        if (on_date_jump)
            on_date_jump(ts_ms);
    };
    header_->on_threads_requested = [this]
    {
        if (on_threads_button_clicked)
        {
            on_threads_button_clicked();
        }
    };
    message_list_->on_thread_preview_clicked =
        [this](const std::string& root_event_id)
    {
        if (on_thread_open_requested)
        {
            on_thread_open_requested(root_event_id);
        }
    };
    // Pin / Unpin hover-button forwarding. The message list raises one of
    // these when the user clicks the per-row 📌 affordance; the shell wires
    // the request to Client::pin_event / Client::unpin_event.
    message_list_->on_pin_requested = [this](const std::string& event_id)
    {
        if (on_pin_requested) on_pin_requested(event_id);
    };
    message_list_->on_unpin_requested = [this](const std::string& event_id)
    {
        if (on_unpin_requested) on_unpin_requested(event_id);
    };
    // Wire header info-requested to show room info panel.
    header_->on_info_requested = [this]() { show_room_info(); };

    // Wire room info panel callbacks.
    room_info_panel_->on_close = [this]()
    {
        room_info_panel_->close();
        if (repaint_requester_) repaint_requester_();
    };
    room_info_panel_->on_fetch_notification_mode = [this](std::string room_id)
    {
        if (on_fetch_notification_mode)
            on_fetch_notification_mode(std::move(room_id));
    };
    room_info_panel_->on_notification_mode_changed =
        [this](std::string room_id, std::string mode)
    {
        if (on_notification_mode_changed)
            on_notification_mode_changed(std::move(room_id), std::move(mode));
    };
    room_info_panel_->on_favourite_changed =
        [this](std::string room_id, bool on)
    {
        if (on_favourite_changed) on_favourite_changed(std::move(room_id), on);
    };
    room_info_panel_->on_low_priority_changed =
        [this](std::string room_id, bool on)
    {
        if (on_low_priority_changed) on_low_priority_changed(std::move(room_id), on);
    };
    room_info_panel_->on_link_clicked = [this](std::string url)
    {
        if (on_link_clicked) on_link_clicked(std::move(url));
    };
    room_info_panel_->on_link_hovered = [this](std::string url)
    {
        if (on_link_hovered) on_link_hovered(std::move(url));
    };
    room_info_panel_->on_fetch_members = [this](std::string room_id)
    {
        if (on_fetch_room_members) on_fetch_room_members(std::move(room_id));
    };
    room_info_panel_->on_save_topic = [this](std::string room_id, std::string t)
    {
        // Optimistically update the header so it reflects the new topic
        // immediately, without waiting for the SDK to echo the state event back.
        if (room_id == current_room_info_.id && header_)
        {
            current_room_info_.topic      = t;
            current_room_info_.topic_html = {};
            header_->set_room(current_room_info_);
        }
        if (on_save_topic) on_save_topic(std::move(room_id), std::move(t));
    };
    room_info_panel_->on_leave_room = [this](std::string room_id)
    {
        // If MainAppWidget supplied a confirm provider, prompt before
        // forwarding the SDK-touching callback. Falls back to firing
        // directly so tests / future hosts that skip wiring still work.
        if (confirm_provider_)
        {
            ConfirmDialog::Options opts;
            const std::string display = current_room_info_.name.empty()
                                            ? "this room"
                                            : current_room_info_.name;
            opts.title          = "Leave " + display + "?";
            opts.body           = "You will stop receiving messages and need "
                                  "to be re-invited to rejoin.";
            opts.confirm_label  = "Leave";
            opts.cancel_label   = "Cancel";
            opts.destructive    = true;

            // Close the room-info panel as we hand off to the confirm
            // overlay — otherwise the prompt would sit on top of the panel
            // and the user would see two backdrops stacked.
            if (room_info_panel_) room_info_panel_->close();
            if (on_layout_changed) on_layout_changed();

            const std::string captured_id = room_id;
            confirm_provider_(std::move(opts), [this, captured_id]() {
                if (on_leave_room) on_leave_room(captured_id);
            });
            return;
        }
        if (on_leave_room) on_leave_room(std::move(room_id));
    };
    room_info_panel_->on_media_view_requested = [this](std::string room_id)
    {
        // Close the panel as we hand off to the gallery — otherwise it
        // stays open (and, since RoomInfoPanel is a RoomView-owned overlay
        // rather than a MainAppWidget-level one, visually underneath) once
        // the gallery closes again.
        if (room_info_panel_) room_info_panel_->close();
        if (on_layout_changed) on_layout_changed();
        if (on_media_view_requested) on_media_view_requested(std::move(room_id));
    };
    room_info_panel_->on_member_clicked =
        [this](std::string uid, std::string name, std::string av)
    {
        show_user_profile(std::move(uid), std::move(name), std::move(av));
    };
    room_info_panel_->on_avatar_clicked =
        [this](std::string url, std::string name)
    {
        if (on_avatar_clicked)
            on_avatar_clicked(std::move(url), std::move(name));
    };
    room_info_panel_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
    room_info_panel_->on_room_settings_requested = [this]()
    {
        show_room_settings();
    };

    // Wire room settings view callbacks.
    room_settings_view_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
    room_settings_view_->on_cancel = [this]()
    {
        room_settings_view_->close();
        if (repaint_requester_) repaint_requester_();
    };
    room_settings_view_->on_avatar_upload_clicked = [this]()
    {
        if (on_room_settings_avatar_upload_requested)
            on_room_settings_avatar_upload_requested(current_room_info_.id);
    };
    room_settings_view_->on_avatar_remove_clicked = [this]()
    {
        room_settings_view_->set_staged_avatar("");
    };
    room_settings_view_->on_copy_to_clipboard = [this](std::string text)
    {
        if (on_set_clipboard) on_set_clipboard(text);
    };

    // Wire user profile panel callbacks.
    user_profile_panel_->on_close = [this]()
    {
        user_profile_panel_->close();
        if (repaint_requester_) repaint_requester_();
    };
    user_profile_panel_->on_open_dm = [this](std::string user_id)
    {
        if (on_open_dm) on_open_dm(std::move(user_id));
    };
    user_profile_panel_->on_check_has_dm = [this](const std::string& user_id)
    {
        return on_has_dm && on_has_dm(user_id);
    };
    user_profile_panel_->on_ignore = [this](std::string user_id)
    {
        if (on_ignore_user) on_ignore_user(std::move(user_id));
    };
    user_profile_panel_->on_avatar_clicked =
        [this](std::string url, std::string name)
    {
        if (on_avatar_clicked)
            on_avatar_clicked(std::move(url), std::move(name));
    };
    user_profile_panel_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
}

// ── Private helpers ────────────────────────────────────────────────────────

void RoomView::show_room_info()
{
    if (!room_info_panel_ || !has_room_)
        return;
    if (user_profile_panel_ && user_profile_panel_->is_open())
        user_profile_panel_->close();
    if (message_list_)
    {
        room_info_panel_->set_media_count(
            count_synced_media_(message_list_->messages()));
    }
    room_info_panel_->open(current_room_info_);
    if (repaint_requester_) repaint_requester_();
}

void RoomView::refresh_media_count_()
{
    if (!room_info_panel_ || !room_info_panel_->is_open() || !message_list_)
        return;
    room_info_panel_->set_media_count(
        count_synced_media_(message_list_->messages()));
}

void RoomView::show_room_settings()
{
    if (!room_settings_view_ || !has_room_)
        return;
    if (room_info_panel_ && room_info_panel_->is_open())
        room_info_panel_->close();
    if (user_profile_panel_ && user_profile_panel_->is_open())
        user_profile_panel_->close();
    room_settings_view_->open(current_room_info_);
    if (on_room_settings_opened) on_room_settings_opened(current_room_info_.id);
    if (repaint_requester_) repaint_requester_();
}

void RoomView::open_user_profile(const std::string& user_id)
{
    show_user_profile(user_id, "", "");
}

void RoomView::show_user_profile(std::string user_id, std::string display_name,
                                  std::string avatar_url)
{
    if (!user_profile_panel_)
        return;
    if (room_info_panel_ && room_info_panel_->is_open())
        room_info_panel_->close();
    user_profile_panel_->open(std::move(user_id), std::move(display_name),
                               std::move(avatar_url));
    if (repaint_requester_) repaint_requester_();
}

void RoomView::set_dm_button_state(UserProfilePanel::DmButtonState state)
{
    if (user_profile_panel_)
        user_profile_panel_->set_dm_button_state(state);
}

void RoomView::close_user_profile()
{
    if (user_profile_panel_ && user_profile_panel_->is_open())
    {
        user_profile_panel_->close();
        if (repaint_requester_) repaint_requester_();
    }
}

// ── Providers ─────────────────────────────────────────────────────────────

void RoomView::set_avatar_provider(MessageListView::ImageProvider p)
{
    stored_avatar_provider_ = p;
    // The same provider goes to the header (room avatar), the panels, the
    // thread view (if already open), and the message list (per-sender avatars).
    // Copy to all except the last recipient; move into the last one.
    if (header_)
    {
        header_->set_avatar_provider(p);
    }
    if (room_info_panel_)
    {
        room_info_panel_->set_avatar_provider(p);
    }
    if (room_settings_view_)
    {
        room_settings_view_->set_avatar_provider(p);
    }
    if (user_profile_panel_)
    {
        user_profile_panel_->set_avatar_provider(p);
    }
    if (thread_view_ && thread_view_->message_list())
    {
        thread_view_->message_list()->set_avatar_provider(p);
    }
    if (message_list_)
    {
        message_list_->set_avatar_provider(std::move(p));
    }
}

void RoomView::set_image_provider(MessageListView::ImageProvider p)
{
    stored_image_provider_ = p;
    if (thread_view_ && thread_view_->message_list())
    {
        thread_view_->message_list()->set_image_provider(p);
    }
    if (message_list_)
    {
        message_list_->set_image_provider(std::move(p));
    }
}

void RoomView::set_image_acquirer(MessageListView::ImageAcquirer a)
{
    stored_image_acquirer_ = a;
    if (thread_view_ && thread_view_->message_list())
    {
        thread_view_->message_list()->set_image_acquirer(a);
    }
    if (message_list_)
    {
        message_list_->set_image_acquirer(std::move(a));
    }
}

void RoomView::set_shortcode_provider(MessageListView::ShortcodeProvider p)
{
    if (message_list_)
    {
        message_list_->set_shortcode_provider(std::move(p));
    }
}

void RoomView::set_preview_provider(MessageListView::PreviewProvider p)
{
    if (message_list_)
    {
        message_list_->set_preview_provider(std::move(p));
    }
}

void RoomView::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    if (message_list_)
    {
        message_list_->set_audio_player(std::move(player));
    }
}

void RoomView::set_voice_bytes_provider(MessageListView::VoiceBytesProvider p)
{
    if (message_list_)
    {
        message_list_->set_voice_bytes_provider(std::move(p));
    }
}

void RoomView::set_repaint_requester(std::function<void()> f)
{
    repaint_requester_ = f;
    if (message_list_)
    {
        message_list_->set_repaint_requester(std::move(f));
    }
}

void RoomView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    post_delayed_ = f;
    if (message_list_)
    {
        message_list_->set_post_delayed(std::move(f));
    }
}

void RoomView::set_confirm_provider(ConfirmProvider p)
{
    confirm_provider_ = std::move(p);
}

bool RoomView::is_overlay_open() const
{
    return (room_settings_view_ && room_settings_view_->is_open()) ||
           (room_info_panel_    && room_info_panel_->is_open()) ||
           (user_profile_panel_ && user_profile_panel_->is_open()) ||
           (room_media_view_    && room_media_view_->is_open());
}

RoomSearchBar* RoomView::room_search_bar() const
{
    return room_search_bar_;
}

void RoomView::open_room_search()
{
    if (!has_room_ || !room_search_bar_)
        return;
    room_search_bar_->set_visible(true);
    room_search_bar_->open();
    if (on_layout_changed)
        on_layout_changed();
}

void RoomView::close_room_search()
{
    if (!room_search_bar_ || !room_search_bar_->is_open())
        return;
    room_search_bar_->close();
    room_search_bar_->set_visible(false);
    if (message_list_)
    {
        message_list_->clear_search_matches();
        message_list_->set_highlighted_event({});
    }
    if (on_room_search_closed)
        on_room_search_closed();
    if (on_layout_changed)
        on_layout_changed();
}

void RoomView::show_call_banner(const std::string& room_id,
                                 const std::string& slot_id,
                                 const std::string& caller_display_name,
                                 const std::string& call_intent,
                                 std::uint64_t      lifetime_ms)
{
    if (!call_banner_) return;

    call_banner_room_id_ = room_id;
    call_banner_slot_id_ = slot_id;

    // Bump the generation counter so any in-flight auto-dismiss fires as a
    // no-op if the user answers or declines before the timeout expires.
    const auto gen = ++call_banner_dismiss_gen_;

    const std::uint64_t effective_lifetime = (lifetime_ms > 0) ? lifetime_ms : 30000;

    call_banner_->set_call(
        caller_display_name,
        call_intent,
        [this, room_id, slot_id] {       // on_answer
            dismiss_call_banner();
            if (on_start_call) on_start_call(room_id, slot_id, false);
        },
        [this] {                          // on_decline
            dismiss_call_banner();
            // The decline button is a focusable Button, so clicking it
            // moved tk-level (and, via claim_native_focus_container_, real
            // native) keyboard focus onto it — then dismiss_call_banner()
            // hides the whole banner out from under that focus, with
            // nothing left to hold it. Land back on the compose box,
            // mirroring the reply/edit-flow refocus calls above.
            compose_bar_->focus();
        }
    );

    // Auto-dismiss after the effective lifetime.
    if (post_delayed_)
    {
        post_delayed_(static_cast<int>(effective_lifetime), [this, gen] {
            if (gen != call_banner_dismiss_gen_) return;
            dismiss_call_banner();
        });
    }

    if (on_layout_changed) on_layout_changed();
}

void RoomView::dismiss_call_banner()
{
    ++call_banner_dismiss_gen_;
    if (call_banner_) call_banner_->clear();
    if (on_layout_changed) on_layout_changed();
}

bool RoomView::call_banner_visible() const
{
    return call_banner_ && call_banner_->visible();
}

views::CallOverlayWidget*
RoomView::mount_call_panel(
    views::CallOverlayWidget::Mode initial_mode,
    PostDelayedFn                  post_delayed,
    std::function<void()>          repaint_requester,
    std::function<const tk::Image*(const std::string&)> avatar_provider,
    std::function<std::string(const std::string&)>      display_name_provider)
{
    if (call_panel_) unmount_call_panel();
    auto w = std::make_unique<views::CallOverlayWidget>();
    w->set_post_delayed(std::move(post_delayed));
    w->set_repaint_requester(std::move(repaint_requester));
    w->set_avatar_provider(std::move(avatar_provider));
    w->set_display_name_provider(std::move(display_name_provider));
    w->set_mode(initial_mode);
    call_panel_ = add_child(std::move(w));
    return call_panel_;
}

void RoomView::unmount_call_panel()
{
    if (!call_panel_) return;
    call_panel_->stop_timer();
    remove_child(call_panel_);
    call_panel_ = nullptr;
}

bool RoomView::room_search_open() const
{
    return room_search_bar_ && room_search_bar_->is_open();
}

void RoomView::set_video_player_factory(MessageListView::VideoPlayerFactory f)
{
    if (message_list_)
    {
        message_list_->set_video_player_factory(std::move(f));
    }
}

void RoomView::set_video_fetch_provider(MessageListView::VideoFetchProvider f)
{
    if (message_list_)
    {
        message_list_->set_video_fetch_provider(std::move(f));
    }
}

// ── Historical-mode helpers ────────────────────────────────────────────────

void RoomView::set_historical_mode(bool historical)
{
    if (message_list_)
    {
        message_list_->set_historical_mode(historical);
    }
}

void RoomView::set_paginating(bool paginating)
{
    if (message_list_)
    {
        message_list_->set_paginating(paginating);
    }
}

void RoomView::set_message_list_relayout_suppressed(bool suppressed)
{
    if (message_list_)
    {
        message_list_->set_relayout_suppressed(suppressed);
    }
}

void RoomView::set_message_list_video_suspended(bool suspended)
{
    if (message_list_)
    {
        message_list_->set_video_playback_suspended(suspended);
    }
}

bool RoomView::scroll_to_event_id(const std::string& id)
{
    return message_list_ && message_list_->scroll_to_event_id(id);
}

// ── Room / message state ───────────────────────────────────────────────────

void RoomView::set_room(const tesseract::RoomInfo& info)
{
    const bool room_changed = info.id != current_room_info_.id;
    if (room_changed)
    {
        if (room_info_panel_)
            room_info_panel_->close();
        if (room_settings_view_)
            room_settings_view_->close();
        if (user_profile_panel_)
            user_profile_panel_->close();
        if (room_media_view_ && room_media_view_->is_open())
            room_media_view_->close();
        close_room_search();
        // The action-pill "more" submenu and the call-type popup are backdrop
        // overlays that only close via their own click-outside handling
        // (on_dismissed); without this they stay open across a room switch
        // until the user happens to click in the new room's timeline. Run
        // the same cleanup on_dismissed does rather than a bare close() so
        // hover-locked rows aren't left stuck.
        if (overflow_menu_ && overflow_menu_->is_open() &&
            overflow_menu_->on_dismissed)
            overflow_menu_->on_dismissed();
        if (call_popup_ && call_popup_->is_open() && call_popup_->on_dismissed)
            call_popup_->on_dismissed();
    }
    else if (room_info_panel_ && room_info_panel_->is_open())
    {
        room_info_panel_->refresh_info(info);
    }
    has_room_ = true;
    current_room_info_ = info;
    if (on_room_avatar_needed)
        on_room_avatar_needed(info);
    // Prefetch members on room change so mention pills (avatar) and mention
    // clicks (display name + avatar) resolve without first opening the
    // room-info panel. Lives here so every shell gets it for free.
    if (room_changed && on_fetch_room_members)
        on_fetch_room_members(info.id);
    if (header_)
    {
        header_->set_room(info);
        header_->set_show_search_btn(true);
    }
    if (compose_bar_)
    {
        compose_bar_->set_enabled(true);
        // Default-focus policy: composing is the primary activity in a chat
        // client, so a genuine room switch (not a same-room metadata
        // refresh — see the else-if above) should land the user in the
        // compose box, ready to type, without them needing to click first.
        // Deferred to paint() rather than called synchronously here: at
        // this point text_area()'s own visible_ flag may still reflect an
        // earlier "no room active" relayout (MainAppWidget::arrange()'s
        // tail force-hides it whenever compose_text_area_rect() is empty)
        // — the *next* relayout is what actually reveals it, and that
        // happens asynchronously, after set_room() has already returned.
        // Calling focus() synchronously here raced that and silently
        // no-op'd via Host::request_focus's visible_in_tree() guard, with
        // nothing ever retrying once the widget became visible.
        if (room_changed)
            pending_default_focus_ = true;
    }
}

void RoomView::clear_room()
{
    has_room_ = false;
    close_room_search();
    if (compose_bar_)
    {
        compose_bar_->set_enabled(false);
    }
    // Drop any pinned-events state from the previous room so it can't bleed
    // into the next set_room() before refresh_pinned_for_current_room_ runs
    // (e.g. account switch → onRoomSelected fast-path skips tab_select_room).
    set_pinned({});
}

void RoomView::set_messages(std::vector<MessageRowData> msgs, bool room_switch)
{
    if (message_list_)
    {
        message_list_->set_messages(std::move(msgs), room_switch);
    }
    refresh_media_count_();
}

void RoomView::insert_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->insert_message(index, std::move(msg));
    }
    refresh_media_count_();
}

void RoomView::update_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->update_message(index, std::move(msg));
    }
    refresh_media_count_();
}

void RoomView::remove_message(std::size_t index)
{
    if (message_list_)
    {
        message_list_->remove_message(index);
    }
    refresh_media_count_();
}

void RoomView::append_message(MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->append_message(std::move(msg));
    }
    refresh_media_count_();
}

void RoomView::prepend_messages(std::vector<MessageRowData> rows)
{
    if (message_list_)
        message_list_->prepend_messages(std::move(rows));
    refresh_media_count_();
}

void RoomView::append_messages(std::vector<MessageRowData> rows)
{
    if (message_list_)
        message_list_->append_messages(std::move(rows));
    refresh_media_count_();
}

void RoomView::notify_image_ready(const std::string& url)
{
    if (message_list_)
    {
        message_list_->notify_image_ready(url);
    }
    if (thread_view_ && thread_view_->visible())
    {
        if (auto* ml = thread_view_->message_list())
            ml->notify_image_ready(url);
    }
}

void RoomView::notify_url_preview_ready(const std::string& url)
{
    if (message_list_)
    {
        message_list_->notify_url_preview_ready(url);
    }
    if (thread_view_ && thread_view_->visible())
    {
        if (auto* ml = thread_view_->message_list())
            ml->notify_url_preview_ready(url);
    }
}

void RoomView::set_typing_text(std::string text)
{
    // The typing indicator is now a synthetic trailing row inside the
    // message list, so it scrolls with the timeline and is hidden when
    // the user scrolls up.
    if (message_list_)
    {
        message_list_->set_typing_text(std::move(text));
    }
}

// ── Compose integration ────────────────────────────────────────────────────

void RoomView::set_text_area_natural_height(float h)
{
    if (compose_bar_)
    {
        compose_bar_->set_text_area_natural_height(h);
    }
}

void RoomView::set_current_text(std::string text)
{
    if (compose_bar_)
    {
        compose_bar_->set_current_text(std::move(text));
    }
}

void RoomView::clear_compose_text()
{
    if (compose_bar_)
    {
        compose_bar_->set_current_text({});
    }
}

bool RoomView::edit_last_own()
{
    if (!compose_bar_ || !message_list_)
    {
        return false;
    }
    // Don't hijack Up while the user is already mid-edit or composing a
    // reply — that would silently discard their in-progress context.
    if (compose_bar_->has_editing() || compose_bar_->has_reply())
    {
        return false;
    }
    return message_list_->edit_last_own();
}

tk::Rect RoomView::compose_text_area_rect() const
{
    // No active room → brand view is shown and the compose bar is not arranged,
    // so its cached rect is stale. Return empty to tell the shell to hide the
    // native text-area overlay.
    if (!has_room_)
        return {};
    // text_area_rect() inside ComposeBar is computed from bounds_ (world
    // coords) so the rect is already in surface space — no offset needed.
    return compose_bar_ ? compose_bar_->text_area_rect() : tk::Rect{};
}

tk::Rect RoomView::compose_bar_rect() const
{
    // Full compose-bar bounds in surface space (used to anchor the full-width
    // GIF strip just above the bar). Empty when no room is active.
    if (!has_room_)
        return {};
    return compose_bar_ ? compose_bar_->bounds() : tk::Rect{};
}

// ── Panel convenience setters ─────────────────────────────────────────────

void RoomView::set_room_members(std::vector<tesseract::RoomMember> members)
{
    room_members_ = members;
    if (room_info_panel_)
        room_info_panel_->set_members(std::move(members));
}

// ── Thread panel ──────────────────────────────────────────────────────────

void RoomView::set_show_threads_button(bool show)
{
    if (header_)
        header_->set_show_threads_btn(show);
}

void RoomView::set_thread_panel(ThreadPanelState state,
                                const std::string& root_event_id)
{
    thread_panel_state_ = state;
    thread_panel_root_  = root_event_id;

    // Lazily instantiate the child widgets the first time each panel mode
    // is requested. Once created they live for the lifetime of the
    // RoomView and toggle visibility via set_visible.
    if (state == ThreadPanelState::List && !thread_list_view_)
    {
        auto tlv = std::make_unique<ThreadListView>();
        thread_list_view_ = add_child(std::move(tlv));
        thread_list_view_->on_close = [this]
        {
            if (on_threads_button_clicked) on_threads_button_clicked();
        };
        thread_list_view_->on_thread_clicked =
            [this](const std::string& root)
        {
            if (on_thread_open_requested) on_thread_open_requested(root);
        };
    }
    if (state == ThreadPanelState::Open && !thread_view_)
    {
        auto tv = std::make_unique<ThreadView>();
        thread_view_ = add_child(std::move(tv));
        // Forward the providers that were set before this lazy creation.
        if (auto* ml = thread_view_->message_list())
        {
            if (stored_avatar_provider_)
                ml->set_avatar_provider(stored_avatar_provider_);
            if (stored_image_provider_)
                ml->set_image_provider(stored_image_provider_);
            if (stored_image_acquirer_)
                ml->set_image_acquirer(stored_image_acquirer_);
            // Wire the same hover-action / media / reaction callbacks as the
            // main timeline so reply / edit / redact (and friends) work
            // inside the thread panel. Reply sends are routed through the
            // thread by the thread_panel_state_ branch in compose_bar_->on_send_reply.
            wire_message_list_callbacks_(ml);
        }
        thread_view_->on_close = [this]
        {
            if (on_thread_close_requested) on_thread_close_requested();
        };
    }

    // Toggle child visibility so the tk pointer-dispatch + paint loop
    // honours the active mode automatically.
    if (thread_view_)
        thread_view_->set_visible(state == ThreadPanelState::Open);
    if (thread_list_view_)
        thread_list_view_->set_visible(state == ThreadPanelState::List);

    // Block hover + click on the main timeline whenever any thread panel
    // is open. The blocker's on_click fires on_thread_close_requested.
    const bool panel_open = state != ThreadPanelState::Closed;
    if (message_blocker_)
        message_blocker_->set_visible(panel_open);
    // Clear any stale hover state on the main timeline so affordances
    // from before the panel opened don't linger under the blocker.
    if (panel_open && message_list_)
        message_list_->on_pointer_leave();

    // Dim the main timeline + highlight the thread root when open.
    if (message_list_)
    {
        message_list_->set_dimmed(state == ThreadPanelState::Open);
        message_list_->set_highlighted_event(
            state == ThreadPanelState::Open ? root_event_id : std::string{});
    }

    if (on_layout_changed) on_layout_changed();
    if (repaint_requester_) repaint_requester_();
}

// ── Pinned events ─────────────────────────────────────────────────────────

void RoomView::set_pinned(std::vector<tesseract::PinnedEvent> pins)
{
    // Always update MessageListView's id-set first so the hover Pin/Unpin
    // button reflects the correct state even before the banner exists.
    std::unordered_set<std::string> ids;
    ids.reserve(pins.size());
    for (const auto& p : pins) ids.insert(p.event_id);
    if (message_list_) message_list_->set_pinned_event_ids(std::move(ids));

    // No banner needed and none ever created → cheap exit.
    if (pins.empty() && !pinned_banner_) return;

    // Lazily create the banner the first time a non-empty pin set arrives.
    // Mirrors the thread_view_ / thread_list_view_ pattern in
    // set_thread_panel(): add_child once, toggle visibility thereafter.
    if (!pinned_banner_)
    {
        auto banner = std::make_unique<PinnedBanner>();
        pinned_banner_ = add_child(std::move(banner));
        pinned_banner_->on_jump_to = [this](const std::string& event_id)
        {
            if (!message_list_) return;
            message_list_->set_highlighted_event(event_id);
            // If the pinned event is already loaded, just scroll. Otherwise
            // route through on_scroll_to_original so the shell rebuilds the
            // timeline focused on the target via subscribe_room_at — same
            // path the reply-quote click uses for off-window events.
            if (message_list_->scroll_to_event_id(event_id)) return;
            if (on_scroll_to_original) on_scroll_to_original(event_id);
        };
    }

    const bool was_visible = pinned_banner_->visible();
    const bool now_visible = !pins.empty();
    pinned_banner_->set_pins(std::move(pins));
    pinned_banner_->set_visible(now_visible);

    // Trigger a relayout if the banner appeared or disappeared — the
    // message_list_ rect depends on whether the banner consumes the row.
    if (was_visible != now_visible)
    {
        if (on_layout_changed) on_layout_changed();
    }
    if (repaint_requester_) repaint_requester_();
}

void RoomView::set_can_pin(bool can_pin)
{
    if (message_list_) message_list_->set_can_pin(can_pin);
}

// ── tk::Widget overrides ───────────────────────────────────────────────────

tk::Size RoomView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // RoomView fills the entire surface it's mounted on.
    return constraints;
}

void RoomView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    if (!has_room_)
    {
        if (brand_view_)
        {
            brand_view_->arrange(ctx, bounds);
        }
        return;
    }

    // Room settings replaces the header/timeline/composer entirely while
    // open — skip laying out the rest of the room content.
    if (room_settings_view_ && room_settings_view_->is_open())
    {
        room_settings_view_->arrange(ctx, bounds);
        return;
    }

    const float compose_h =
        compose_bar_ ? compose_bar_->natural_height() : ComposeBar::kMinHeight;

    const float header_h = header_
                               ? header_->measure(ctx, {bounds.w, bounds.h}).h
                               : RoomHeader::kHeight;
    const float header_bottom = bounds.y + header_h;
    const float compose_top = bounds.y + bounds.h - compose_h;

    // Thread panel floats on top of the main column: header, message list,
    // and compose bar always arrange at full width. The panel paints last
    // and intercepts pointer events first (children dispatch in reverse),
    // so the message list keeps its natural width — text doesn't reflow
    // when the panel toggles.
    //
    // The pinned-events banner (when visible) consumes a kBannerH-tall
    // strip at the top of the message-list area. The banner spans the
    // full bounds.w just like the message list does, so when the thread
    // panel opens it floats on top of both header + banner + list.
    float list_top = header_bottom;
    const bool banner_visible =
        pinned_banner_ && pinned_banner_->visible() &&
        !pinned_banner_->pins().empty();
    if (banner_visible)
    {
        const float bh = PinnedBanner::kBannerH;
        pinned_banner_->arrange(ctx, {bounds.x, list_top, bounds.w, bh});
        list_top += bh;
    }

    // Incoming-call banner — occupies kBannerH between the pinned banner and
    // the search bar / message list when visible.
    if (call_banner_ && call_banner_->visible())
    {
        call_banner_->arrange(ctx, {bounds.x, list_top, bounds.w,
                                    IncomingCallBanner::kBannerH});
        list_top += IncomingCallBanner::kBannerH;
    }

    // Docked call panel — occupies kDockedH between banners and message list.
    // DockedExpanded collapses the message area entirely.
    if (call_panel_ && call_panel_->visible())
    {
        const auto mode = call_panel_->mode();
        if (mode == views::CallOverlayWidget::Mode::DockedExpanded)
        {
            const float panel_h = bounds.bottom() - list_top;
            call_panel_->arrange(ctx, {bounds.x, list_top, bounds.w, panel_h});
            list_top = bounds.bottom(); // collapse messages
        }
        else // Docked (220 px strip)
        {
            constexpr float kDockedH = 220.0f;
            call_panel_->arrange(ctx, {bounds.x, list_top, bounds.w, kDockedH});
            list_top += kDockedH;
        }
    }

    // In-room search bar — occupies kStripH between the header/banner and the
    // message list when open. Arranged at zero height (invisible to
    // hit-testing) when closed.
    const bool search_bar_open = room_search_bar_ && room_search_bar_->is_open();
    if (room_search_bar_)
    {
        if (search_bar_open)
        {
            room_search_bar_->arrange(
                ctx, {bounds.x, list_top, bounds.w, RoomSearchBar::kStripH});
            list_top += RoomSearchBar::kStripH;
        }
        else
        {
            room_search_bar_->arrange(ctx, {bounds.x, list_top, bounds.w, 0.0f});
        }
    }

    const float msg_h = std::max(0.0f, compose_top - list_top);

    if (header_)
    {
        header_->arrange(ctx, {bounds.x, bounds.y, bounds.w, header_h});
    }

    if (message_list_)
    {
        message_list_->arrange(ctx, {bounds.x, list_top, bounds.w, msg_h});
    }

    if (message_blocker_)
    {
        message_blocker_->arrange(ctx,
                                  {bounds.x, list_top, bounds.w, msg_h});
    }

    if (compose_bar_)
    {
        compose_bar_->arrange(ctx,
                              {bounds.x, compose_top, bounds.w, compose_h});
    }

    // Right-anchored floating panel. Fixed `kThreadPanelWidth`, clamped to
    // the room bounds so narrow windows still get a sensible overlay (it
    // just covers the message list — and the pinned banner, when visible —
    // entirely). Only one of {list, view} is visible at a time. Always
    // anchored from header_bottom (not list_top) so it covers the banner
    // strip too, keeping it hidden while the thread panel is up.
    const float panel_w = std::min(kThreadPanelWidth, bounds.w);
    const float panel_x = bounds.x + bounds.w - panel_w;
    const float panel_h = std::max(0.0f, compose_top - header_bottom);
    if (thread_list_view_ && thread_panel_state_ == ThreadPanelState::List)
    {
        thread_list_view_->arrange(
            ctx, {panel_x, header_bottom, panel_w, panel_h});
    }
    if (thread_view_ && thread_panel_state_ == ThreadPanelState::Open)
    {
        thread_view_->arrange(
            ctx, {panel_x, header_bottom, panel_w, panel_h});
    }

    // Overlay panels fill the full bounds (painted on top of all other
    // children). Always arranged, even closed, so each can zero its own
    // hit-testable area — see overlay_panels_()'s doc comment.
    for (tk::Widget* panel : overlay_panels_())
    {
        if (panel)
            panel->arrange(ctx, bounds);
    }
}

void RoomView::paint(tk::PaintCtx& ctx)
{
    // Scope Tab/Shift-Tab traversal to whichever overlay panel is open (if
    // any) — mirrors active_overlay_panel_()'s existing role for pointer
    // dispatch, just for keyboard focus instead. Re-synced every paint
    // rather than pushed from each panel's open()/close(), so it can't drift
    // out of sync and covers all five panels uniformly.
    if (ctx.host)
    {
        if (tk::Widget* o = active_overlay_panel_())
            ctx.host->set_focus_scope(o);
        else
            ctx.host->clear_focus_scope();
    }

    if (!has_room_)
    {
        if (brand_view_)
        {
            brand_view_->paint(ctx);
        }
        return;
    }

    if (room_settings_view_ && room_settings_view_->is_open())
    {
        room_settings_view_->paint(ctx);
        return;
    }

    // Consume the deferred default-focus request from set_room() — by now
    // this frame's full arrange() pass (including MainAppWidget's tail
    // visibility gating) has already settled compose_bar_'s text_area()
    // visibility, so this either succeeds or safely no-ops (e.g. a modal
    // opened in the meantime) via Host::request_focus's own guards.
    if (pending_default_focus_)
    {
        pending_default_focus_ = false;
        if (compose_bar_)
            compose_bar_->focus();
    }

    // Each child paints its own background; the typing indicator now lives
    // inside the message list (synthetic trailing row), so RoomView paints
    // no strip of its own.
    if (header_)
    {
        header_->paint(ctx);
    }
    if (pinned_banner_ && pinned_banner_->visible() &&
        !pinned_banner_->pins().empty())
    {
        pinned_banner_->paint(ctx);
    }
    if (call_banner_ && call_banner_->visible())
        call_banner_->paint(ctx);
    if (room_search_bar_ && room_search_bar_->is_open())
        room_search_bar_->paint(ctx);
    if (message_list_)
    {
        message_list_->paint(ctx);
    }

    if (compose_bar_)
    {
        compose_bar_->paint(ctx);
        if (drag_hover_)
            tk::paint_drag_hover_highlight(ctx, compose_bar_->bounds());
    }

    // Paint call panel last so it always draws on top of both message list and
    // compose bar (critical in DockedExpanded mode where it fills the area).
    if (call_panel_ && call_panel_->visible())
        call_panel_->paint(ctx);

    // Right-side thread panel.
    if (thread_list_view_ && thread_panel_state_ == ThreadPanelState::List)
        thread_list_view_->paint(ctx);
    if (thread_view_ && thread_panel_state_ == ThreadPanelState::Open)
        thread_view_->paint(ctx);

    // Each of these self-gates on its own is_open()/open_ state — see
    // overlay_panels_()'s doc comment.
    for (tk::Widget* panel : overlay_panels_())
    {
        if (panel)
            panel->paint(ctx);
    }
}

std::array<tk::Widget*, 5> RoomView::overlay_panels_() const
{
    return {room_info_panel_, user_profile_panel_, overflow_menu_,
            call_popup_, room_media_view_};
}

// ── Pointer/hit-test routing ────────────────────────────────────────────────
//
// The overlay panels are arranged at the full RoomView bounds and consume
// backdrop clicks, so delegating to the open one both blocks pass-through to
// the widgets it covers (pinned banner, thread panel) and still lets the
// panel's own children (avatar, member rows, buttons) work. When no panel is
// open we fall through to the base traversal, leaving normal dispatch intact.

tk::Widget* RoomView::active_overlay_panel_() const
{
    // Mutually exclusive in practice (show_room_info / show_room_settings /
    // show_user_profile all close each other, and RoomInfoPanel closes
    // itself before firing on_media_view_requested); prefer the one painted
    // last if more than one is somehow open — the reverse of
    // overlay_panels_()'s paint order, plus room_settings_view_ (which
    // isn't in that list — see its own doc comment).
    if (room_media_view_ && room_media_view_->is_open())
        return room_media_view_;
    if (room_settings_view_ && room_settings_view_->is_open())
        return room_settings_view_;
    if (user_profile_panel_ && user_profile_panel_->is_open())
        return user_profile_panel_;
    if (room_info_panel_ && room_info_panel_->is_open())
        return room_info_panel_;
    if (overflow_menu_ && overflow_menu_->is_open())
        return overflow_menu_;
    if (call_popup_ && call_popup_->is_open())
        return call_popup_;
    return nullptr;
}

tk::Widget* RoomView::hit_test(tk::Point world)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->hit_test(world);
    return tk::Widget::hit_test(world);
}

tk::Widget* RoomView::dispatch_pointer_down(tk::Point world)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_pointer_down(world);
    if (tk::Widget* hit = tk::Widget::dispatch_pointer_down(world))
        return hit;
    // Nothing in the room claimed the click (blank timeline/header space,
    // gaps between rows, etc.) and it's genuinely within our own bounds —
    // redirect it to the compose box rather than losing focus to nothing,
    // matching the "click anywhere, just start typing" chat-app
    // convention. text_area() is itself focusable() (already gates on
    // enabled_) and has a no-op on_pointer_up, so handing it back here is
    // exactly as if the click had landed on it directly — Host's own
    // existing post-dispatch request_focus(pressed) does the rest.
    if (has_room_ && contains_world(world) && compose_bar_)
        return compose_bar_->text_area();
    return nullptr;
}

tk::Widget* RoomView::dispatch_pointer_move(tk::Point world, bool* dirty)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_pointer_move(world, dirty);
    return tk::Widget::dispatch_pointer_move(world, dirty);
}

tk::Widget* RoomView::dispatch_right_click(tk::Point world)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_right_click(world);
    return tk::Widget::dispatch_right_click(world);
}

bool RoomView::dispatch_wheel(tk::Point world, float dx, float dy)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_wheel(world, dx, dy);
    return tk::Widget::dispatch_wheel(world, dx, dy);
}

tk::Widget* RoomView::dispatch_file_drop(tk::Point world, tk::FileDropPayload& payload)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_file_drop(world, payload);
    return tk::Widget::dispatch_file_drop(world, payload);
}

bool RoomView::on_file_drop(tk::Point /*local*/, tk::FileDropPayload& payload)
{
    if (!compose_bar_ || !compose_bar_->enabled())
        return false;
    const std::uint64_t limit =
        media_upload_limit_provider ? media_upload_limit_provider() : 0;
    auto outcome = route_file_drop_to_compose_bar(
        *compose_bar_, std::move(payload.bytes), std::move(payload.mime),
        std::move(payload.filename), limit, media_info_extractor);
    if (on_file_drop_outcome)
        on_file_drop_outcome(outcome);
    return outcome == FileDropOutcome::Accepted;
}

tk::Widget* RoomView::dispatch_drag_hover(tk::Point world)
{
    if (tk::Widget* o = active_overlay_panel_())
        return o->dispatch_drag_hover(world);
    return tk::Widget::dispatch_drag_hover(world);
}

bool RoomView::on_drag_hover(tk::Point /*local*/)
{
    if (!compose_bar_ || !compose_bar_->enabled())
        return false;
    drag_hover_ = true;
    return true;
}

void RoomView::on_drag_leave()
{
    drag_hover_ = false;
}

} // namespace tesseract::views
