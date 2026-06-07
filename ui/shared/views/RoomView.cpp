#include "RoomView.h"

#include "icons.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace tesseract::views
{

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

    auto compose = std::make_unique<ComposeBar>();
    compose_bar_ = add_child(std::move(compose));
    compose_bar_->set_enabled(false);

    auto room_info = std::make_unique<RoomInfoPanel>();
    room_info_panel_ = add_child(std::move(room_info));

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
        if (on_reply_focus) on_reply_focus();
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
                   bool can_delete, bool can_pin, bool is_pinned)
    {
        // anchor is in world coordinates — PopupMenu::open() takes world coords.
        std::vector<PopupMenu::Item> items;
        if (can_delete)
        {
            items.push_back({"", // SVG icon below
                             kRedactSvg,
                             "Delete message", /*destructive=*/true,
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
                             is_pinned ? "Unpin message" : "Pin message",
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
        if (on_reply_focus) on_reply_focus();
    };
    ml->on_link_hovered = [this](const std::string& url)
    {
        if (on_link_hovered) on_link_hovered(url);
    };
    ml->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip) on_show_tooltip(std::move(text), anchor);
    };
    ml->on_hide_tooltip = [this]()
    {
        if (on_hide_tooltip) on_hide_tooltip();
    };
    ml->on_receipt_needed = [this](const std::string& event_id)
    {
        if (on_receipt_needed) on_receipt_needed(event_id);
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
    compose_bar_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip)
            on_show_tooltip(std::move(text), anchor);
    };
    compose_bar_->on_hide_tooltip = [this]
    {
        if (on_hide_tooltip)
            on_hide_tooltip();
    };
    compose_bar_->on_focus_input = [this]
    {
        if (on_focus_input)
            on_focus_input();
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
        if (on_reply_focus)
        {
            on_reply_focus();
        }
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
    header_->on_jump_to_date_requested = [this]
    {
        if (on_jump_to_date_requested)
        {
            on_jump_to_date_requested();
        }
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
    header_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip)
        {
            on_show_tooltip(std::move(text), anchor);
        }
    };
    header_->on_hide_tooltip = [this]
    {
        if (on_hide_tooltip)
        {
            on_hide_tooltip();
        }
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
    room_info_panel_->on_show_tooltip =
        [this](std::string text, tk::Rect anchor)
    {
        if (on_show_tooltip) on_show_tooltip(std::move(text), anchor);
    };
    room_info_panel_->on_hide_tooltip = [this]()
    {
        if (on_hide_tooltip) on_hide_tooltip();
    };
    room_info_panel_->on_fetch_members = [this](std::string room_id)
    {
        if (on_fetch_room_members) on_fetch_room_members(std::move(room_id));
    };
    room_info_panel_->on_save_topic = [this](std::string room_id, std::string t)
    {
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
    room_info_panel_->open(current_room_info_);
    if (repaint_requester_) repaint_requester_();
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
    return (room_info_panel_    && room_info_panel_->is_open()) ||
           (user_profile_panel_ && user_profile_panel_->is_open());
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
        if (user_profile_panel_)
            user_profile_panel_->close();
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
    }
    if (compose_bar_)
    {
        compose_bar_->set_enabled(true);
    }
}

void RoomView::clear_room()
{
    has_room_ = false;
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
}

void RoomView::insert_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->insert_message(index, std::move(msg));
    }
}

void RoomView::update_message(std::size_t index, MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->update_message(index, std::move(msg));
    }
}

void RoomView::remove_message(std::size_t index)
{
    if (message_list_)
    {
        message_list_->remove_message(index);
    }
}

void RoomView::append_message(MessageRowData msg)
{
    if (message_list_)
    {
        message_list_->append_message(std::move(msg));
    }
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

tk::Rect RoomView::topic_edit_rect() const
{
    return room_info_panel_ ? room_info_panel_->topic_edit_rect() : tk::Rect{};
}

bool RoomView::topic_edit_visible() const
{
    return room_info_panel_ && room_info_panel_->topic_edit_visible();
}

void RoomView::set_topic_edit_text(std::string t)
{
    if (room_info_panel_)
        room_info_panel_->set_topic_edit_text(std::move(t));
}

std::string RoomView::topic_edit_initial_text() const
{
    return room_info_panel_ ? room_info_panel_->topic_edit_initial_text() : std::string{};
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

    // Overlay panels fill the full bounds (painted on top of all other children).
    if (room_info_panel_)
        room_info_panel_->arrange(ctx, bounds);
    if (user_profile_panel_)
        user_profile_panel_->arrange(ctx, bounds);
    // Overflow popup: always arranged so it can zero its bounds when closed.
    if (overflow_menu_)
        overflow_menu_->arrange(ctx, bounds);
}

void RoomView::paint(tk::PaintCtx& ctx)
{
    if (!has_room_)
    {
        if (brand_view_)
        {
            brand_view_->paint(ctx);
        }
        return;
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
    if (message_list_)
    {
        message_list_->paint(ctx);
    }

    if (compose_bar_)
    {
        compose_bar_->paint(ctx);
    }

    // Right-side thread panel.
    if (thread_list_view_ && thread_panel_state_ == ThreadPanelState::List)
        thread_list_view_->paint(ctx);
    if (thread_view_ && thread_panel_state_ == ThreadPanelState::Open)
        thread_view_->paint(ctx);

    if (room_info_panel_ && room_info_panel_->is_open())
        room_info_panel_->paint(ctx);
    if (user_profile_panel_ && user_profile_panel_->is_open())
        user_profile_panel_->paint(ctx);
    if (overflow_menu_ && overflow_menu_->is_open())
        overflow_menu_->paint(ctx);
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
    // Mutually exclusive in practice (show_room_info / show_user_profile close
    // each other); prefer the one painted last if both are somehow open.
    if (user_profile_panel_ && user_profile_panel_->is_open())
        return user_profile_panel_;
    if (room_info_panel_ && room_info_panel_->is_open())
        return room_info_panel_;
    if (overflow_menu_ && overflow_menu_->is_open())
        return overflow_menu_;
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
    return tk::Widget::dispatch_pointer_down(world);
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

} // namespace tesseract::views
